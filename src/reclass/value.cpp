#include "reclass/value.hpp"

#include <charconv>
#include <cstring>
#include <format>
#include <limits>

namespace slopkit::reclass
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

        struct IntegerLiteral
        {
            std::uint64_t magnitude {};
            bool          negative {};
            bool          hex {};
        };

        std::expected<IntegerLiteral, std::string> parse_literal(std::string_view text, bool force_hex)
        {
            text = trim(text);
            if (text.empty())
            {
                return std::unexpected(std::string {"empty value"});
            }

            IntegerLiteral literal;
            if (text.front() == '+' || text.front() == '-')
            {
                literal.negative = text.front() == '-';
                text.remove_prefix(1);
            }
            if (!text.empty() && text.front() == '#')
            {
                literal.hex = false;
                text.remove_prefix(1);
            }
            else if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
            {
                literal.hex = true;
                text.remove_prefix(2);
            }
            else
            {
                literal.hex = force_hex;
            }
            if (text.empty())
            {
                return std::unexpected(std::string {"malformed number"});
            }

            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), literal.magnitude, literal.hex ? 16 : 10);
            if (error != std::errc {} || end != text.data() + text.size())
            {
                return std::unexpected(std::string {"malformed number"});
            }
            return literal;
        }

        std::uint64_t read_bytes(std::span<const std::byte> bytes, std::size_t width)
        {
            std::uint64_t value = 0;
            for (std::size_t index = 0; index < width; ++index)
            {
                value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[index])) << (8 * index);
            }
            return value;
        }

        std::int64_t sign_extend(std::uint64_t magnitude, std::size_t bits)
        {
            if (bits >= 64)
            {
                return static_cast<std::int64_t>(magnitude);
            }
            const std::uint64_t mask  = (std::uint64_t {1} << bits) - 1;
            const std::uint64_t value = magnitude & mask;
            if ((value & (std::uint64_t {1} << (bits - 1))) != 0)
            {
                return static_cast<std::int64_t>(value | ~mask);
            }
            return static_cast<std::int64_t>(value);
        }

        std::vector<std::byte> little_endian(std::uint64_t value, std::size_t width)
        {
            std::vector<std::byte> bytes(width);
            for (std::size_t index = 0; index < width; ++index)
            {
                bytes[index] = static_cast<std::byte>((value >> (8 * index)) & 0xFF);
            }
            return bytes;
        }

        std::size_t integer_bits(NodeKind kind) noexcept
        {
            return kind_size(kind) * 8;
        }

        std::string out_of_range(NodeKind kind)
        {
            return std::format("out of range for {}", describe(kind));
        }

        std::string lower_case(std::string_view text)
        {
            std::string result {text};
            for (char& character : result)
            {
                if (character >= 'A' && character <= 'Z')
                {
                    character = static_cast<char>(character - 'A' + 'a');
                }
            }
            return result;
        }
    }

    std::string format_node_value(NodeKind kind, std::span<const std::byte> bytes, bool hex)
    {
        const std::size_t size = kind_size(kind);
        if (size == 0 || bytes.size() < size)
        {
            return {};
        }

        switch (kind)
        {
        case NodeKind::int8:
        case NodeKind::int16:
        case NodeKind::int32:
        case NodeKind::int64:
        {
            const std::int64_t value = sign_extend(read_bytes(bytes, size), integer_bits(kind));
            if (!hex)
            {
                return std::format("{}", value);
            }
            std::uint64_t magnitude = static_cast<std::uint64_t>(value);
            if (integer_bits(kind) < 64)
            {
                magnitude &= (std::uint64_t {1} << integer_bits(kind)) - 1;
            }
            return std::format("{:0{}X}", magnitude, size * 2);
        }
        case NodeKind::uint8:
        case NodeKind::uint16:
        case NodeKind::uint32:
        case NodeKind::uint64:
        {
            const std::uint64_t value = read_bytes(bytes, size);
            if (!hex)
            {
                return std::format("{}", value);
            }
            return std::format("{:0{}X}", value, size * 2);
        }
        case NodeKind::float32:
        {
            if (hex)
            {
                float bits {};
                std::memcpy(&bits, bytes.data(), sizeof(bits));
                std::uint32_t raw {};
                std::memcpy(&raw, &bits, sizeof(raw));
                return std::format("{:08X}", raw);
            }
            float value {};
            std::memcpy(&value, bytes.data(), sizeof(value));
            return std::format("{}", value);
        }
        case NodeKind::float64:
        {
            if (hex)
            {
                double bits {};
                std::memcpy(&bits, bytes.data(), sizeof(bits));
                std::uint64_t raw {};
                std::memcpy(&raw, &bits, sizeof(raw));
                return std::format("{:016X}", raw);
            }
            double value {};
            std::memcpy(&value, bytes.data(), sizeof(value));
            return std::format("{}", value);
        }
        case NodeKind::boolean:
            return bytes.front() == std::byte {0} ? "false" : "true";
        case NodeKind::character:
        {
            const auto value = std::to_integer<unsigned char>(bytes.front());
            if (value >= 0x20 && value < 0x7F)
            {
                return std::string(1, static_cast<char>(value));
            }
            return std::format("\\x{:02X}", value);
        }
        case NodeKind::hex8:
        case NodeKind::hex16:
        case NodeKind::hex32:
        case NodeKind::hex64:
        {
            std::string text;
            text.reserve(size * 2);
            for (std::size_t index = 0; index < size; ++index)
            {
                text += std::format("{:02X}", std::to_integer<unsigned char>(bytes[index]));
            }
            return text;
        }
        case NodeKind::pointer:
        case NodeKind::class_pointer:
            return std::format("0x{:X}", read_bytes(bytes, size));
        case NodeKind::class_instance:
            break;
        }
        return {};
    }

    std::expected<std::vector<std::byte>, std::string> parse_node_value(NodeKind kind, std::string_view text, bool hex)
    {
        switch (kind)
        {
        case NodeKind::int8:
        case NodeKind::int16:
        case NodeKind::int32:
        case NodeKind::int64:
        {
            const auto literal = parse_literal(text, hex);
            if (!literal)
            {
                return std::unexpected(literal.error());
            }

            std::int64_t value = 0;
            if (literal->negative)
            {
                const std::uint64_t limit = std::uint64_t {1} << (integer_bits(kind) - 1);
                if (literal->magnitude > limit)
                {
                    return std::unexpected(out_of_range(kind));
                }
                if (integer_bits(kind) == 64 && literal->magnitude == limit)
                {
                    value = std::numeric_limits<std::int64_t>::min();
                }
                else
                {
                    value = -static_cast<std::int64_t>(literal->magnitude);
                }
            }
            else if (literal->hex)
            {
                value = sign_extend(literal->magnitude, integer_bits(kind));
            }
            else
            {
                const std::uint64_t maximum = (std::uint64_t {1} << (integer_bits(kind) - 1)) - 1;
                if (literal->magnitude > maximum)
                {
                    return std::unexpected(out_of_range(kind));
                }
                value = static_cast<std::int64_t>(literal->magnitude);
            }
            return little_endian(static_cast<std::uint64_t>(value), kind_size(kind));
        }
        case NodeKind::uint8:
        case NodeKind::uint16:
        case NodeKind::uint32:
        case NodeKind::uint64:
        {
            const auto literal = parse_literal(text, hex);
            if (!literal)
            {
                return std::unexpected(literal.error());
            }
            if (literal->negative)
            {
                return std::unexpected(std::format("{} is unsigned", describe(kind)));
            }

            const std::size_t   size = kind_size(kind);
            const std::uint64_t mask =
                size >= 8 ? std::numeric_limits<std::uint64_t>::max() : (std::uint64_t {1} << (size * 8)) - 1;
            const std::uint64_t magnitude = literal->magnitude;
            if (!literal->hex && magnitude > mask)
            {
                return std::unexpected(out_of_range(kind));
            }
            return little_endian(magnitude & mask, size);
        }
        case NodeKind::float32:
        case NodeKind::float64:
        {
            if (hex)
            {
                const auto literal = parse_literal(text, true);
                if (!literal)
                {
                    return std::unexpected(literal.error());
                }
                if (literal->negative)
                {
                    return std::unexpected(std::format("{} is unsigned", describe(kind)));
                }
                const std::size_t   size = kind_size(kind);
                const std::uint64_t mask =
                    size >= 8 ? std::numeric_limits<std::uint64_t>::max() : (std::uint64_t {1} << (size * 8)) - 1;
                return little_endian(literal->magnitude & mask, size);
            }

            const std::string_view trimmed = trim(text);
            if (trimmed.empty())
            {
                return std::unexpected(std::string {"empty value"});
            }

            double value = 0.0;
            const auto [end, error] =
                std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), value, std::chars_format::general);
            if (error != std::errc {} || end != trimmed.data() + trimmed.size())
            {
                return std::unexpected(std::string {"malformed floating-point number"});
            }

            if (kind == NodeKind::float32)
            {
                const float            real = static_cast<float>(value);
                std::vector<std::byte> bytes(sizeof(real));
                std::memcpy(bytes.data(), &real, sizeof(real));
                return bytes;
            }
            std::vector<std::byte> bytes(sizeof(value));
            std::memcpy(bytes.data(), &value, sizeof(value));
            return bytes;
        }
        case NodeKind::boolean:
        {
            const std::string trimmed = lower_case(trim(text));
            if (trimmed == "true" || trimmed == "1")
            {
                return little_endian(1, 1);
            }
            if (trimmed == "false" || trimmed == "0")
            {
                return little_endian(0, 1);
            }
            return std::unexpected(std::string {"expected true or false"});
        }
        case NodeKind::character:
        {
            const std::string_view trimmed = trim(text);
            if (trimmed.size() >= 3 && trimmed.front() == '\\' && (trimmed[1] == 'x' || trimmed[1] == 'X'))
            {
                std::uint64_t magnitude = 0;
                const auto [end, error] =
                    std::from_chars(trimmed.data() + 2, trimmed.data() + trimmed.size(), magnitude, 16);
                if (error != std::errc {} || end != trimmed.data() + trimmed.size() || magnitude > 0xFF)
                {
                    return std::unexpected(std::string {"malformed character escape"});
                }
                return little_endian(magnitude, 1);
            }
            if (trimmed.size() != 1)
            {
                return std::unexpected(std::string {"expected a single character"});
            }
            return little_endian(static_cast<unsigned char>(trimmed.front()), 1);
        }
        case NodeKind::hex8:
        case NodeKind::hex16:
        case NodeKind::hex32:
        case NodeKind::hex64:
        {
            const std::size_t size = kind_size(kind);

            std::string_view rest = trim(text);
            if (rest.size() > 2 && rest[0] == '0' && (rest[1] == 'x' || rest[1] == 'X'))
            {
                rest.remove_prefix(2);
            }
            std::string digits;
            for (const char character : rest)
            {
                if (character != ' ' && character != '\t')
                {
                    digits.push_back(character);
                }
            }
            if (digits.size() != size * 2)
            {
                return std::unexpected(std::format("expected {} hex digits", size * 2));
            }

            std::vector<std::byte> bytes(size);
            for (std::size_t index = 0; index < size; ++index)
            {
                std::uint64_t pair = 0;
                const auto [end, error] =
                    std::from_chars(digits.data() + 2 * index, digits.data() + 2 * index + 2, pair, 16);
                if (error != std::errc {} || end != digits.data() + 2 * index + 2)
                {
                    return std::unexpected(std::string {"malformed hex digits"});
                }
                bytes[index] = static_cast<std::byte>(pair);
            }
            return bytes;
        }
        case NodeKind::pointer:
        case NodeKind::class_pointer:
        {
            const auto literal = parse_literal(text, true);
            if (!literal)
            {
                return std::unexpected(literal.error());
            }
            if (literal->negative)
            {
                return std::unexpected(std::string {"address must not be negative"});
            }
            return little_endian(literal->magnitude, 8);
        }
        case NodeKind::class_instance:
            break;
        }
        return std::unexpected(std::string {"a class instance has no value"});
    }

}
