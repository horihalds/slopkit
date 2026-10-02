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

        std::expected<AddressEntry, std::string> parse_entry(std::string_view text, int line_number)
        {
            AddressEntry entry;
            std::size_t  position = 0;
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
                else
                {
                    return std::unexpected(std::format("line {}: unknown key '{}'", line_number, key));
                }
            }
            return entry;
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
            return std::unexpected("cannot open " + path.string() + " for writing");
        }

        file << "slopkit-table 1\n";
        for (const auto& entry : table.entries())
        {
            file << std::format("entry description=\"{}\" address=0x{:X} type={} frozen={} hex={} value={}\n",
                                escape(entry.description),
                                entry.address,
                                type_token(entry.type),
                                entry.active ? 1 : 0,
                                entry.hex ? 1 : 0,
                                hex_bytes(entry.bytes));
        }
        if (!file)
        {
            return std::unexpected("write failed for " + path.string());
        }
        return {};
    }

    std::expected<void, std::string> load(const std::filesystem::path& path, AddressTable& table)
    {
        std::ifstream file(path);
        if (!file)
        {
            return std::unexpected("cannot open " + path.string());
        }

        AddressTable loaded;
        std::string  line;
        int          line_number = 0;
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
            if (!view.starts_with("entry"))
            {
                return std::unexpected(std::format("line {}: expected an entry line", line_number));
            }
            auto entry = parse_entry(view.substr(5), line_number);
            if (!entry)
            {
                return std::unexpected(entry.error());
            }
            loaded.add(std::move(*entry));
        }
        if (file.bad())
        {
            return std::unexpected("read failed for " + path.string());
        }

        table = std::move(loaded);
        return {};
    }

} // namespace slopkit::table
