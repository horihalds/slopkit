#include "table/serializer.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "expr/expression.hpp"
#include "expr/resolver.hpp"
#include "table/entry_name.hpp"
#include "table/table_zip.hpp"

namespace slopkit::table
{

    namespace
    {
        constexpr std::string_view kFormatToken         = "slopkit-table 4";
        // Reader compatibility: a version-3 archive predates script entries, so
        // it simply has no `.lua` members. The writer always emits the current
        // token, and an older build refuses it explicitly instead of
        // mis-parsing a Lua body as a value entry.
        constexpr std::string_view kPreviousFormatToken = "slopkit-table 3";

        std::string_view trim(std::string_view value)
        {
            const auto first = value.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos)
            {
                return {};
            }
            const auto last = value.find_last_not_of(" \t\r\n");
            return value.substr(first, last - first + 1);
        }

        std::string escape(std::string_view value)
        {
            std::string result;
            result.reserve(value.size());
            for (const char character : value)
            {
                switch (character)
                {
                case '"':
                    result += "\\\"";
                    break;
                case '\\':
                    result += "\\\\";
                    break;
                case '\n':
                    result += "\\n";
                    break;
                default:
                    result += character;
                    break;
                }
            }
            return result;
        }

        std::optional<scan::ValueType> type_from_token(std::string_view token)
        {
            constexpr std::pair<std::string_view, scan::ValueType> kTypes[] = {
                {   "i8",       scan::ValueType::byte},
                {  "i16",      scan::ValueType::int16},
                {  "i32",      scan::ValueType::int32},
                {  "i64",      scan::ValueType::int64},
                {  "f32",    scan::ValueType::float32},
                {  "f64",    scan::ValueType::float64},
                {  "str",     scan::ValueType::string},
                {"bytes", scan::ValueType::byte_array},
                // The removed "All" entry was a 4-byte integer scan; tables
                // saved while it existed still load as 4 Bytes.
                {  "all",      scan::ValueType::int32},
            };
            for (const auto& [name, type] : kTypes)
            {
                if (name == token)
                {
                    return type;
                }
            }
            return std::nullopt;
        }

        // Runs the key/value token loop over a member body, calling `visit` for
        // every token. A broken token returns an error naming the member; the
        // visitor may reject a key or a value as well.
        template<typename Visitor>
        std::expected<void, std::string>
        parse_key_values(std::string_view text, std::string_view context, Visitor&& visit)
        {
            std::size_t position = 0;
            while (position < text.size())
            {
                while (position < text.size() && (text[position] == ' ' || text[position] == '\t'))
                {
                    ++position;
                }
                if (position >= text.size())
                {
                    break;
                }

                const auto equals = text.find('=', position);
                if (equals == std::string_view::npos)
                {
                    return std::unexpected(std::format("{}: missing '='", context));
                }
                const std::string key(text.substr(position, equals - position));
                position = equals + 1;

                std::string value;
                if (position < text.size() && text[position] == '"')
                {
                    ++position;
                    while (position < text.size() && text[position] != '"')
                    {
                        if (text[position] == '\\' && position + 1 < text.size())
                        {
                            ++position;
                            switch (text[position])
                            {
                            case 'n':
                                value += '\n';
                                break;
                            case '"':
                                value += '"';
                                break;
                            case '\\':
                                value += '\\';
                                break;
                            default:
                                value += text[position];
                                break;
                            }
                        }
                        else
                        {
                            value += text[position];
                        }
                        ++position;
                    }
                    if (position >= text.size())
                    {
                        return std::unexpected(std::format("{}: unterminated string", context));
                    }
                    ++position; // closing quote
                }
                else
                {
                    const auto end = text.find_first_of(" \t", position);
                    value          = std::string(
                        text.substr(position, end == std::string_view::npos ? std::string_view::npos : end - position));
                    position = end == std::string_view::npos ? text.size() : end;
                }

                auto result = visit(key, std::move(value));
                if (!result)
                {
                    return result;
                }
            }
            return {};
        }

        std::optional<bool> parse_bool(std::string_view text)
        {
            if (text == "1")
            {
                return true;
            }
            if (text == "0")
            {
                return false;
            }
            return std::nullopt;
        }

        // Parses one entry body (`type=… hex=… size=… expr="…"`). The description
        // is the member name and is not part of the body; the frozen flag and the
        // cached value bytes are not persisted.
        std::expected<AddressEntry, std::string> parse_entry_body(std::string_view text, std::string_view member)
        {
            AddressEntry entry;
            std::size_t  size      = 0;
            bool         seen_type = false;
            bool         seen_size = false;
            bool         seen_hex  = false;
            bool         seen_expr = false;

            const auto visit = [&](const std::string& key, std::string value) -> std::expected<void, std::string>
            {
                if (key == "type")
                {
                    const auto type = type_from_token(value);
                    if (!type)
                    {
                        return std::unexpected(std::format("{}: unknown type '{}'", member, value));
                    }
                    entry.type = *type;
                    seen_type  = true;
                }
                else if (key == "size")
                {
                    std::size_t parsed      = 0;
                    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
                    if (error != std::errc {} || end != value.data() + value.size())
                    {
                        return std::unexpected(std::format("{}: invalid size '{}'", member, value));
                    }
                    size      = parsed;
                    seen_size = true;
                }
                else if (key == "hex")
                {
                    const auto flag = parse_bool(value);
                    if (!flag)
                    {
                        return std::unexpected(std::format("{}: invalid boolean '{}'", member, value));
                    }
                    entry.hex = *flag;
                    seen_hex  = true;
                }
                else if (key == "expr")
                {
                    entry.expression = std::move(value);
                    seen_expr        = true;
                }
                else
                {
                    return std::unexpected(std::format("{}: unknown key '{}'", member, key));
                }
                return {};
            };

            auto parsed = parse_key_values(text, member, visit);
            if (!parsed)
            {
                return std::unexpected(parsed.error());
            }
            if (!seen_type)
            {
                return std::unexpected(std::format("{}: missing 'type'", member));
            }
            if (!seen_size)
            {
                return std::unexpected(std::format("{}: missing 'size'", member));
            }
            if (!seen_hex)
            {
                return std::unexpected(std::format("{}: missing 'hex'", member));
            }
            if (!seen_expr)
            {
                return std::unexpected(std::format("{}: missing 'expr'", member));
            }

            entry.bytes.assign(size, std::byte {0});
            return entry;
        }

        std::expected<TableSettings, std::string> parse_settings(std::string_view text, std::string_view member)
        {
            TableSettings settings;
            const auto    visit = [&](const std::string& key, std::string value) -> std::expected<void, std::string>
            {
                if (key == "target")
                {
                    settings.target_process = std::move(value);
                }
                else if (key == "exe_path")
                {
                    settings.exe_path = std::move(value);
                }
                else if (key == "auto_attach")
                {
                    const auto flag = parse_bool(value);
                    if (!flag)
                    {
                        return std::unexpected(std::format("{}: invalid boolean '{}'", member, value));
                    }
                    settings.auto_attach = *flag;
                }
                else if (key == "match_exe_path")
                {
                    const auto flag = parse_bool(value);
                    if (!flag)
                    {
                        return std::unexpected(std::format("{}: invalid boolean '{}'", member, value));
                    }
                    settings.match_exe_path = *flag;
                }
                else
                {
                    return std::unexpected(std::format("{}: unknown key '{}'", member, key));
                }
                return {};
            };
            auto parsed = parse_key_values(text, member, visit);
            if (!parsed)
            {
                return std::unexpected(parsed.error());
            }
            return settings;
        }

        // First member with `name`, or nullptr. The archive is read into memory
        // first, so the returned pointer stays valid for the caller's scope.
        const std::string* find_member(const std::vector<ArchiveMember>& members, std::string_view name)
        {
            const auto found = std::ranges::find(members, name, &ArchiveMember::name);
            return found == members.end() ? nullptr : &found->text;
        }

        // The reader used to evaluate a pointer-free expression without a target;
        // it is never reached when `pointer_levels()` is 0.
        std::expected<std::uint64_t, std::string> no_pointer_reader(std::uint64_t)
        {
            return std::unexpected(std::string {"a pointer read was required"});
        }
    } // namespace

    std::string_view type_token(scan::ValueType type) noexcept
    {
        switch (type)
        {
        case scan::ValueType::byte:
            return "i8";
        case scan::ValueType::int16:
            return "i16";
        case scan::ValueType::int32:
            return "i32";
        case scan::ValueType::int64:
            return "i64";
        case scan::ValueType::float32:
            return "f32";
        case scan::ValueType::float64:
            return "f64";
        case scan::ValueType::string:
            return "str";
        case scan::ValueType::byte_array:
            return "bytes";
        }
        return "i32";
    }

    std::expected<void, std::string> save(const std::filesystem::path& path, const AddressTable& table)
    {
        const TableSettings& settings = table.settings();

        std::vector<ArchiveMember> members;
        members.reserve(table.size() + 3);
        members.push_back(ArchiveMember {.name = "version.txt", .text = std::string(kFormatToken) + "\n"});
        if (!settings.empty())
        {
            members.push_back(ArchiveMember {
                .name = "settings.txt",
                .text = std::format("target=\"{}\" exe_path=\"{}\" auto_attach={} match_exe_path={}\n",
                                    escape(settings.target_process),
                                    escape(settings.exe_path),
                                    settings.auto_attach ? 1 : 0,
                                    settings.match_exe_path ? 1 : 0),
            });
        }

        std::vector<std::string> taken;
        std::vector<std::string> order;
        taken.reserve(table.size());
        order.reserve(table.size());
        for (const AddressEntry& entry : table.entries())
        {
            const bool        script = entry.kind == EntryKind::script;
            const std::string name =
                entry_member_name(entry.description, taken, script ? kScriptMemberExtension : kValueMemberExtension);
            taken.push_back(name);

            const std::string member = std::string(kEntriesPrefix) + name;
            if (script)
            {
                // A script member holds the Lua source and nothing else: no
                // header and no added newline.
                members.push_back(ArchiveMember {.name = member, .text = entry.script});
            }
            else
            {
                // An entry that was never given an expression still has to be
                // reachable; its address is written as the hex literal it already is.
                const std::string expression =
                    entry.expression.empty() ? std::format("{:X}", entry.address) : entry.expression;
                members.push_back(ArchiveMember {
                    .name = member,
                    .text = std::format("type={} hex={} size={} expr=\"{}\"\n",
                                        type_token(entry.type),
                                        entry.hex ? 1 : 0,
                                        entry.bytes.size(),
                                        escape(expression)),
                });
            }
            order.push_back(member);
        }

        std::string index;
        for (const std::string& line : order)
        {
            index += line;
            index += '\n';
        }
        members.push_back(ArchiveMember {.name = "index.txt", .text = std::move(index)});

        const auto written = write_archive(path, members);
        if (!written)
        {
            log::warning(log::category::table, std::format("cannot write {}: {}", path.string(), written.error()));
            return std::unexpected(written.error());
        }
        log::info(log::category::table, std::format("saved {} entry/entries to {}", table.size(), path.string()));
        return {};
    }

    std::expected<void, std::string> load(const std::filesystem::path& path, AddressTable& table)
    {
        if (!is_zip_archive(path))
        {
            const std::string message = std::format("{}: not a slopkit table archive", path.string());
            log::warning(log::category::table, message);
            return std::unexpected(message);
        }

        auto archive = read_archive(path);
        if (!archive)
        {
            const std::string message = std::format("{}: {}", path.string(), archive.error());
            log::warning(log::category::table, message);
            return std::unexpected(archive.error());
        }
        const std::vector<ArchiveMember>& members = *archive;

        const std::string* version = find_member(members, "version.txt");
        if (version == nullptr)
        {
            const std::string message = std::format("{}: version.txt: missing", path.string());
            log::warning(log::category::table, message);
            return std::unexpected(std::string {"version.txt: missing"});
        }
        if (const std::string_view version_text = trim(*version);
            version_text != kFormatToken && version_text != kPreviousFormatToken)
        {
            const std::string message =
                std::format("{}: version.txt: unknown format '{}'", path.string(), version_text);
            log::warning(log::category::table, message);
            return std::unexpected(std::format("version.txt: unknown format '{}'", version_text));
        }

        AddressTable loaded;
        if (const std::string* raw_settings = find_member(members, "settings.txt"); raw_settings != nullptr)
        {
            auto settings = parse_settings(trim(*raw_settings), "settings.txt");
            if (!settings)
            {
                log::warning(log::category::table, std::format("{}: {}", path.string(), settings.error()));
                return std::unexpected(settings.error());
            }
            loaded.settings() = std::move(*settings);
        }

        const std::string* index = find_member(members, "index.txt");
        if (index == nullptr)
        {
            const std::string message = std::format("{}: index.txt: missing", path.string());
            log::warning(log::category::table, message);
            return std::unexpected(std::string {"index.txt: missing"});
        }

        std::vector<AddressEntry> entries;
        std::vector<std::string>  listed;
        std::size_t               position = 0;
        while (position <= index->size())
        {
            const auto             newline = index->find('\n', position);
            const std::string_view raw     = std::string_view {*index}.substr(
                position, newline == std::string::npos ? std::string::npos : newline - position);
            const std::string_view line = trim(raw);
            if (!line.empty())
            {
                const std::string  member(line);
                const std::string* body = find_member(members, member);
                if (body == nullptr)
                {
                    const std::string message = std::format("{}: index.txt: missing entry '{}'", path.string(), member);
                    log::warning(log::category::table, message);
                    return std::unexpected(std::format("index.txt: missing entry '{}'", member));
                }

                const std::optional<EntryKind> kind = kind_for_member(member);
                if (!kind)
                {
                    const std::string message = std::format("{}: {}: unknown entry extension", path.string(), member);
                    log::warning(log::category::table, message);
                    return std::unexpected(std::format("{}: unknown entry extension", member));
                }
                if (*kind == EntryKind::script)
                {
                    // A script member's body is the Lua source verbatim: the
                    // member name is the description and nothing has to be
                    // parsed out of the body.
                    AddressEntry script;
                    script.kind        = EntryKind::script;
                    script.description = member_stem(member);
                    script.script      = *body;
                    entries.push_back(std::move(script));
                }
                else
                {
                    auto entry = parse_entry_body(trim(*body), member);
                    if (!entry)
                    {
                        log::warning(log::category::table, std::format("{}: {}", path.string(), entry.error()));
                        return std::unexpected(entry.error());
                    }
                    entry->description = member_stem(member);

                    const auto expression = expr::parse(entry->expression);
                    if (!expression)
                    {
                        const std::string message = std::format("{}: invalid expr '{}'", member, entry->expression);
                        log::warning(log::category::table, std::format("{}: {}", path.string(), message));
                        return std::unexpected(message);
                    }
                    // A literal base resolves without a target; a module or pointer
                    // expression is left at 0 for the panel's resolve pass.
                    if (expression->pointer_levels() == 0)
                    {
                        const auto resolved =
                            expr::evaluate(*expression, expr::Modules {}, expr::Symbols {}, no_pointer_reader);
                        if (resolved)
                        {
                            entry->address = *resolved;
                        }
                    }

                    entries.push_back(std::move(*entry));
                }
                listed.push_back(member);
            }
            if (newline == std::string::npos)
            {
                break;
            }
            position = newline + 1;
        }

        // An `entries/` member the index does not list is a damaged archive: the
        // order list is the only source of order, so a stray file is an error.
        for (const ArchiveMember& member : members)
        {
            if (member.name.starts_with("entries/") && std::ranges::find(listed, member.name) == listed.end())
            {
                const std::string message = std::format("{}: {}: not listed in index.txt", path.string(), member.name);
                log::warning(log::category::table, message);
                return std::unexpected(std::format("{}: not listed in index.txt", member.name));
            }
        }

        loaded.replace(std::move(entries));
        table = std::move(loaded);
        log::info(log::category::table, std::format("loaded {} entry/entries from {}", table.size(), path.string()));
        return {};
    }

} // namespace slopkit::table
