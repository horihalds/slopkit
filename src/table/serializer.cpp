#include "table/serializer.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "core/log_categories.hpp"

namespace slopkit::table
{

    namespace
    {
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

        std::string hex_bytes(std::span<const std::byte> bytes)
        {
            std::string result;
            result.reserve(bytes.size() * 2);
            for (const std::byte value : bytes)
            {
                result += std::format("{:02X}", static_cast<unsigned>(value));
            }
            return result;
        }

        std::optional<std::vector<std::byte>> parse_hex_bytes(std::string_view text)
        {
            if (text.size() % 2 != 0)
            {
                return std::nullopt;
            }
            std::vector<std::byte> bytes;
            bytes.reserve(text.size() / 2);
            for (std::size_t i = 0; i < text.size(); i += 2)
            {
                unsigned value          = 0;
                const auto [end, error] = std::from_chars(text.data() + i, text.data() + i + 2, value, 16);
                if (error != std::errc {} || end != text.data() + i + 2)
                {
                    return std::nullopt;
                }
                bytes.push_back(static_cast<std::byte>(value));
            }
            return bytes;
        }

        std::optional<std::uint64_t> parse_hex_u64(std::string_view text)
        {
            if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
            {
                text.remove_prefix(2);
            }
            std::uint64_t value     = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
            if (error != std::errc {} || end != text.data() + text.size())
            {
                return std::nullopt;
            }
            return value;
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
                {  "all",        scan::ValueType::all},
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

        // Runs the key/value token loop over a payload line, calling `visit` for
        // every token. A broken token returns an error naming the line; the
        // visitor may reject a key or a value as well.
        template<typename Visitor>
        std::expected<void, std::string> parse_key_values(std::string_view text, int line_number, Visitor&& visit)
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
                    return std::unexpected(std::format("line {}: missing '='", line_number));
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
                        return std::unexpected(std::format("line {}: unterminated string", line_number));
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

        std::expected<AddressEntry, std::string> parse_entry(std::string_view text, int line_number)
        {
            AddressEntry entry;
            const auto   visit = [&entry, line_number](const std::string& key,
                                                       std::string        value) -> std::expected<void, std::string>
            {
                if (key == "description")
                {
                    entry.description = std::move(value);
                }
                else if (key == "address")
                {
                    const auto address = parse_hex_u64(value);
                    if (!address)
                    {
                        return std::unexpected(std::format("line {}: invalid address '{}'", line_number, value));
                    }
                    entry.address = *address;
                }
                else if (key == "type")
                {
                    const auto type = type_from_token(value);
                    if (!type)
                    {
                        return std::unexpected(std::format("line {}: unknown type '{}'", line_number, value));
                    }
                    entry.type = *type;
                }
                else if (key == "frozen")
                {
                    entry.active = value == "1";
                }
                else if (key == "hex")
                {
                    entry.hex = value == "1";
                }
                else if (key == "value")
                {
                    auto bytes = parse_hex_bytes(value);
                    if (!bytes)
                    {
                        return std::unexpected(std::format("line {}: invalid value '{}'", line_number, value));
                    }
                    entry.bytes = std::move(*bytes);
                }
                else if (key == "expr")
                {
                    entry.expression = std::move(value);
                }
                else
                {
                    return std::unexpected(std::format("line {}: unknown key '{}'", line_number, key));
                }
                return {};
            };
            auto parsed = parse_key_values(text, line_number, visit);
            if (!parsed)
            {
                return std::unexpected(parsed.error());
            }
            return entry;
        }

        std::expected<TableSettings, std::string> parse_settings(std::string_view text, int line_number)
        {
            TableSettings settings;
            const auto    visit = [&settings, line_number](const std::string& key,
                                                           std::string        value) -> std::expected<void, std::string>
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
                        return std::unexpected(std::format("line {}: invalid boolean '{}'", line_number, value));
                    }
                    settings.auto_attach = *flag;
                }
                else if (key == "match_exe_path")
                {
                    const auto flag = parse_bool(value);
                    if (!flag)
                    {
                        return std::unexpected(std::format("line {}: invalid boolean '{}'", line_number, value));
                    }
                    settings.match_exe_path = *flag;
                }
                else
                {
                    return std::unexpected(std::format("line {}: unknown key '{}'", line_number, key));
                }
                return {};
            };
            auto parsed = parse_key_values(text, line_number, visit);
            if (!parsed)
            {
                return std::unexpected(parsed.error());
            }
            return settings;
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
        case scan::ValueType::all:
            return "all";
        }
        return "i32";
    }

    std::expected<void, std::string> save(const std::filesystem::path& path, const AddressTable& table)
    {
        std::ofstream file(path, std::ios::trunc);
        if (!file)
        {
            log::warning(log::category::table, std::format("cannot open {} for writing", path.string()));
            return std::unexpected("cannot open " + path.string() + " for writing");
        }

        file << "slopkit-table 2\n";
        const TableSettings& settings = table.settings();
        if (!settings.empty())
        {
            file << std::format("settings target=\"{}\" exe_path=\"{}\" auto_attach={} match_exe_path={}\n",
                                escape(settings.target_process),
                                escape(settings.exe_path),
                                settings.auto_attach ? 1 : 0,
                                settings.match_exe_path ? 1 : 0);
        }
        for (const auto& entry : table.entries())
        {
            file << std::format("entry description=\"{}\" address={:X} type={} frozen={} hex={} value={}",
                                escape(entry.description),
                                entry.address,
                                type_token(entry.type),
                                entry.active ? 1 : 0,
                                entry.hex ? 1 : 0,
                                hex_bytes(entry.bytes));
            if (!entry.expression.empty())
            {
                file << std::format(" expr=\"{}\"", escape(entry.expression));
            }
            file << '\n';
        }
        if (!file)
        {
            log::warning(log::category::table, std::format("write failed for {}", path.string()));
            return std::unexpected("write failed for " + path.string());
        }
        log::info(log::category::table, std::format("saved {} entry/entries to {}", table.size(), path.string()));
        return {};
    }

    std::expected<void, std::string> load(const std::filesystem::path& path, AddressTable& table)
    {
        std::ifstream file(path);
        if (!file)
        {
            log::warning(log::category::table, std::format("cannot open {}", path.string()));
            return std::unexpected("cannot open " + path.string());
        }

        AddressTable              loaded;
        std::vector<AddressEntry> parsed;
        std::string               line;
        int                       line_number = 0;
        while (std::getline(file, line))
        {
            ++line_number;
            const std::string_view view = trim(line);
            if (view.empty() || view.starts_with('#'))
            {
                continue;
            }
            if (view.starts_with("slopkit-table"))
            {
                continue;
            }
            if (view.starts_with("settings"))
            {
                auto settings = parse_settings(view.substr(8), line_number);
                if (!settings)
                {
                    log::warning(log::category::table, std::format("{}: {}", path.string(), settings.error()));
                    return std::unexpected(settings.error());
                }
                loaded.settings() = std::move(*settings);
                continue;
            }
            if (!view.starts_with("entry"))
            {
                log::warning(log::category::table,
                             std::format("{}: line {}: expected an entry line", path.string(), line_number));
                return std::unexpected(std::format("line {}: expected an entry line", line_number));
            }
            auto entry = parse_entry(view.substr(5), line_number);
            if (!entry)
            {
                log::warning(log::category::table, std::format("{}: {}", path.string(), entry.error()));
                return std::unexpected(entry.error());
            }
            parsed.push_back(std::move(*entry));
        }
        if (file.bad())
        {
            log::warning(log::category::table, std::format("read failed for {}", path.string()));
            return std::unexpected("read failed for " + path.string());
        }

        loaded.replace(std::move(parsed));
        table = std::move(loaded);
        log::info(log::category::table, std::format("loaded {} entry/entries from {}", table.size(), path.string()));
        return {};
    }

} // namespace slopkit::table
