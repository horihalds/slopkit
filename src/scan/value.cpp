#include "scan/value.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <limits>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "expr/expression.hpp"

namespace slopkit::scan
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

        // Magnitude of an integer literal plus how it was written.
        struct IntegerLiteral
        {
            std::uint64_t magnitude {};
            bool          negative {};
            bool          hex {};
        };

        std::expected<IntegerLiteral, ValueError> parse_integer_literal(std::string_view text, bool force_hex)
        {
            text = trim(text);
            if (text.empty())
            {
                return std::unexpected(ValueError {"empty value"});
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
                return std::unexpected(ValueError {"malformed number"});
            }

            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), literal.magnitude, literal.hex ? 16 : 10);
            if (error != std::errc {} || end != text.data() + text.size())
            {
                return std::unexpected(ValueError {"malformed number"});
            }
            return literal;
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

        std::size_t integer_bits(ValueType type) noexcept
        {
            switch (type)
            {
            case ValueType::byte:
                return 8;
            case ValueType::int16:
                return 16;
            case ValueType::int32:
                return 32;
            case ValueType::int64:
                return 64;
            default:
                return 64;
            }
        }

        // Converts an integer literal into the signed value of `type`, wrapping
        // hex input to the type's width (0xFFFFFFFF on 4-byte reads as -1).
        std::expected<std::int64_t, ValueError> integer_value(ValueType type, std::string_view text, bool force_hex)
        {
            auto literal = parse_integer_literal(text, force_hex);
            if (!literal)
            {
                return std::unexpected(literal.error());
            }

            if (literal->negative)
            {
                const auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;
                if (literal->magnitude > limit)
                {
                    return std::unexpected(ValueError {"out of range"});
                }
                if (literal->magnitude == limit)
                {
                    return std::numeric_limits<std::int64_t>::min();
                }
                return -static_cast<std::int64_t>(literal->magnitude);
            }

            if (literal->hex)
            {
                return sign_extend(literal->magnitude, integer_bits(type));
            }

            const std::int64_t value   = static_cast<std::int64_t>(literal->magnitude);
            const std::int64_t minimum = type == ValueType::byte  ? std::numeric_limits<std::int8_t>::min()
                                       : type == ValueType::int16 ? std::numeric_limits<std::int16_t>::min()
                                       : type == ValueType::int32 ? std::numeric_limits<std::int32_t>::min()
                                                                  : std::numeric_limits<std::int64_t>::min();
            const std::int64_t maximum = type == ValueType::byte  ? std::numeric_limits<std::int8_t>::max()
                                       : type == ValueType::int16 ? std::numeric_limits<std::int16_t>::max()
                                       : type == ValueType::int32 ? std::numeric_limits<std::int32_t>::max()
                                                                  : std::numeric_limits<std::int64_t>::max();
            if (literal->magnitude > static_cast<std::uint64_t>(maximum) || value < minimum)
            {
                return std::unexpected(ValueError {"out of range for the selected value type"});
            }
            return value;
        }

        std::expected<double, ValueError> real_value(std::string_view text, bool force_hex)
        {
            text = trim(text);
            if (force_hex)
            {
                return std::unexpected(ValueError {"hexadecimal is not valid for a floating-point type"});
            }
            if (text.empty())
            {
                return std::unexpected(ValueError {"empty value"});
            }
            double value = 0.0;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
            if (error != std::errc {} || end != text.data() + text.size())
            {
                return std::unexpected(ValueError {"malformed floating-point number"});
            }
            return value;
        }

        std::expected<std::vector<std::byte>, ValueError> byte_array_value(std::string_view text, bool force_hex)
        {
            std::vector<std::byte> bytes;
            std::size_t            start = 0;
            while (start < text.size())
            {
                const auto separator = text.find_first_of(" \t\r\n,;", start);
                const auto token     = trim(text.substr(
                    start, separator == std::string_view::npos ? std::string_view::npos : separator - start));
                if (!token.empty())
                {
                    auto literal = parse_integer_literal(token, force_hex);
                    if (!literal)
                    {
                        return std::unexpected(literal.error());
                    }
                    if (literal->negative || literal->magnitude > 0xFF)
                    {
                        return std::unexpected(ValueError {"byte values must be in 0..255"});
                    }
                    bytes.push_back(static_cast<std::byte>(literal->magnitude));
                }
                if (separator == std::string_view::npos)
                {
                    break;
                }
                start = separator + 1;
            }
            if (bytes.empty())
            {
                return std::unexpected(ValueError {"no byte values given"});
            }
            return bytes;
        }

        std::int64_t read_integer(std::span<const std::byte> bytes, ValueType type)
        {
            switch (type)
            {
            case ValueType::byte:
            {
                std::int8_t value {};
                std::memcpy(&value, bytes.data(), sizeof(value));
                return value;
            }
            case ValueType::int16:
            {
                std::int16_t value {};
                std::memcpy(&value, bytes.data(), sizeof(value));
                return value;
            }
            case ValueType::int32:
            {
                std::int32_t value {};
                std::memcpy(&value, bytes.data(), sizeof(value));
                return value;
            }
            case ValueType::int64:
            {
                std::int64_t value {};
                std::memcpy(&value, bytes.data(), sizeof(value));
                return value;
            }
            default:
                return 0;
            }
        }

        double read_real(std::span<const std::byte> bytes, ValueType type)
        {
            if (type == ValueType::float32)
            {
                float value {};
                std::memcpy(&value, bytes.data(), sizeof(value));
                return value;
            }
            double value {};
            std::memcpy(&value, bytes.data(), sizeof(value));
            return value;
        }

        std::size_t format_width(ValueType type) noexcept
        {
            const std::size_t size = value_size(type);
            return size == 0 ? 8 : size * 2;
        }

        // Renders a signed value as bare uppercase hex truncated to the width
        // of its type.
        std::string format_hex(std::int64_t value, ValueType type)
        {
            std::uint64_t     magnitude = static_cast<std::uint64_t>(value);
            const std::size_t bits      = integer_bits(type);
            if (bits < 64)
            {
                magnitude &= (std::uint64_t {1} << bits) - 1;
            }
            return std::format("{:0{}X}", magnitude, format_width(type));
        }

        // Renders a signed value as bare uppercase hex masked to the width of
        // its type, with no zero padding.
        std::string format_hex_minimal(std::int64_t value, ValueType type)
        {
            std::uint64_t     magnitude = static_cast<std::uint64_t>(value);
            const std::size_t bits      = integer_bits(type);
            if (bits < 64)
            {
                magnitude &= (std::uint64_t {1} << bits) - 1;
            }
            return std::format("{:X}", magnitude);
        }
    } // namespace

    std::expected<ScanValue, ValueError> parse_value(ValueType type, std::string_view text, bool hex)
    {
        switch (type)
        {
        case ValueType::string:
            return ScanValue {std::string(text)};
        case ValueType::byte_array:
        {
            auto bytes = byte_array_value(text, hex);
            if (!bytes)
            {
                log::debug(log::category::scan,
                           std::format("rejected {} value '{}': {}", describe(type), text, bytes.error().message));
                return std::unexpected(bytes.error());
            }
            return ScanValue {std::move(*bytes)};
        }
        case ValueType::float32:
        case ValueType::float64:
        {
            auto value = real_value(text, hex);
            if (!value)
            {
                log::debug(log::category::scan,
                           std::format("rejected {} value '{}': {}", describe(type), text, value.error().message));
                return std::unexpected(value.error());
            }
            return ScanValue {*value};
        }
        case ValueType::byte:
        case ValueType::int16:
        case ValueType::int32:
        case ValueType::int64:
        {
            auto value = integer_value(type, text, hex);
            if (!value)
            {
                log::debug(log::category::scan,
                           std::format("rejected {} value '{}': {}", describe(type), text, value.error().message));
                return std::unexpected(value.error());
            }
            return ScanValue {*value};
        }
        }
        return std::unexpected(ValueError {"unsupported value type"});
    }

    std::expected<std::uint64_t, ValueError> parse_address(std::string_view text)
    {
        // Bare digits and `0x…` read as hex, `#…` as decimal; a leading sign is
        // peeled off so a negative address keeps its own rejection message.
        std::string_view digits   = trim(text);
        const bool       negative = !digits.empty() && digits.front() == '-';
        if (!digits.empty() && (digits.front() == '+' || digits.front() == '-'))
        {
            digits.remove_prefix(1);
        }

        const auto literal = expr::parse_literal(digits);
        if (!literal)
        {
            log::debug(log::category::scan, std::format("rejected address '{}': {}", text, literal.error().message));
            return std::unexpected(ValueError {literal.error().message});
        }
        if (negative)
        {
            log::debug(log::category::scan, std::format("rejected address '{}': address must not be negative", text));
            return std::unexpected(ValueError {"address must not be negative"});
        }
        return *literal;
    }

    std::expected<std::uint64_t, ValueError> parse_alignment(std::string_view text)
    {
        text = trim(text);
        if (text.empty())
        {
            return std::unexpected(ValueError {"empty alignment"});
        }
        auto literal = parse_integer_literal(text, false);
        if (!literal || literal->negative || literal->magnitude == 0)
        {
            log::debug(log::category::scan, std::format("rejected alignment '{}'", text));
            return std::unexpected(ValueError {"alignment must be a positive number"});
        }
        return literal->magnitude;
    }

    std::string format_value(ValueType type, std::span<const std::byte> bytes, bool hex)
    {
        switch (type)
        {
        case ValueType::string:
            return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        case ValueType::byte_array:
        {
            std::string result;
            for (std::size_t i = 0; i < bytes.size(); ++i)
            {
                if (i != 0)
                {
                    result += ' ';
                }
                result += std::format("{:02X}", static_cast<unsigned>(bytes[i]));
            }
            return result;
        }
        case ValueType::float32:
        {
            if (bytes.size() < sizeof(float))
            {
                return {};
            }
            // Format at the stored width so the shortest decimal that reads
            // back to the same bits is printed: a Float scan of 3.14 shows
            // "3.14", not the double promotion 3.140000104904175.
            float value {};
            std::memcpy(&value, bytes.data(), sizeof(value));
            return std::format("{}", value);
        }
        case ValueType::float64:
        {
            if (bytes.size() < sizeof(double))
            {
                return {};
            }
            return std::format("{}", read_real(bytes, type));
        }
        case ValueType::byte:
        case ValueType::int16:
        case ValueType::int32:
        case ValueType::int64:
        {
            if (bytes.size() < value_size(type))
            {
                return {};
            }
            const std::int64_t value = read_integer(bytes, type);
            if (hex)
            {
                return format_hex(value, type);
            }
            return std::format("{}", value);
        }
        }
        return {};
    }

    std::optional<std::string> convert_value_base(ValueType type, std::string_view text, bool from_hex, bool to_hex)
    {
        const std::string_view trimmed = trim(text);
        if (trimmed.empty())
        {
            return std::nullopt;
        }

        auto parsed = parse_value(type, trimmed, from_hex);
        if (!parsed)
        {
            return std::nullopt;
        }

        if (std::holds_alternative<std::int64_t>(*parsed))
        {
            const std::int64_t value = std::get<std::int64_t>(*parsed);
            return to_hex ? format_hex_minimal(value, type) : std::format("{}", value);
        }

        if (std::holds_alternative<std::vector<std::byte>>(*parsed))
        {
            const auto& bytes = std::get<std::vector<std::byte>>(*parsed);
            std::string result;
            for (std::size_t i = 0; i < bytes.size(); ++i)
            {
                if (i != 0)
                {
                    result += ' ';
                }
                const auto byte = static_cast<unsigned>(bytes[i]);
                result += to_hex ? std::format("{:X}", byte) : std::format("{}", byte);
            }
            return result;
        }

        return std::nullopt;
    }

    std::vector<std::byte> encode_value(ValueType type, const ScanValue& value)
    {
        if (std::holds_alternative<std::int64_t>(value))
        {
            const std::uint64_t    integer = static_cast<std::uint64_t>(std::get<std::int64_t>(value));
            const std::size_t      width   = value_size(type) == 0 ? 4 : value_size(type);
            std::vector<std::byte> bytes(width);
            for (std::size_t i = 0; i < width; ++i)
            {
                bytes[i] = static_cast<std::byte>((integer >> (8 * i)) & 0xFF);
            }
            return bytes;
        }
        if (std::holds_alternative<double>(value))
        {
            const double converted = std::get<double>(value);
            if (type == ValueType::float32)
            {
                const float            real = static_cast<float>(converted);
                std::vector<std::byte> bytes(sizeof(real));
                std::memcpy(bytes.data(), &real, sizeof(real));
                return bytes;
            }
            std::vector<std::byte> bytes(sizeof(converted));
            std::memcpy(bytes.data(), &converted, sizeof(converted));
            return bytes;
        }
        if (std::holds_alternative<std::string>(value))
        {
            const auto&            text = std::get<std::string>(value);
            std::vector<std::byte> bytes(text.size());
            std::memcpy(bytes.data(), text.data(), text.size());
            return bytes;
        }
        return std::get<std::vector<std::byte>>(value);
    }

    bool matches(ScanType                   scan_type,
                 ValueType                  value_type,
                 std::span<const std::byte> candidate,
                 const ScanValue&           value,
                 const ScanValue&           value_upper,
                 bool /*hex*/)
    {
        if (value_type == ValueType::float32 || value_type == ValueType::float64)
        {
            if (candidate.size() < value_size(value_type) || !std::holds_alternative<double>(value))
            {
                return false;
            }
            // Compare at the stored width, exactly like Matcher::build does, so
            // a Float needle of 3.14 matches 3.14f rather than only the doubles
            // that happen to round-trip through 32 bits.
            const double current = read_real(candidate, value_type);
            const double wanted  = value_type == ValueType::float32
                                     ? static_cast<double>(static_cast<float>(std::get<double>(value)))
                                     : std::get<double>(value);
            switch (scan_type)
            {
            case ScanType::exact_value:
                return current == wanted;
            case ScanType::bigger_than:
                return current > wanted;
            case ScanType::smaller_than:
                return current < wanted;
            case ScanType::value_between:
                return std::holds_alternative<double>(value_upper) && current >= wanted
                    && current <= std::get<double>(value_upper);
            default:
                return false;
            }
        }

        if (value_type == ValueType::string || value_type == ValueType::byte_array)
        {
            if (scan_type != ScanType::exact_value)
            {
                return false;
            }
            if (value_type == ValueType::string)
            {
                if (!std::holds_alternative<std::string>(value))
                {
                    return false;
                }
                const auto& needle = std::get<std::string>(value);
                return candidate.size() >= needle.size()
                    && std::equal(needle.begin(),
                                  needle.end(),
                                  candidate.begin(),
                                  [](char left, std::byte right)
                                  {
                                      return static_cast<unsigned char>(left) == static_cast<unsigned char>(right);
                                  });
            }
            if (!std::holds_alternative<std::vector<std::byte>>(value))
            {
                return false;
            }
            const auto& needle = std::get<std::vector<std::byte>>(value);
            return candidate.size() >= needle.size() && std::equal(needle.begin(), needle.end(), candidate.begin());
        }

        if (!std::holds_alternative<std::int64_t>(value) || candidate.size() < value_size(value_type))
        {
            return false;
        }
        const std::int64_t current = read_integer(candidate, value_type);
        const std::int64_t wanted  = std::get<std::int64_t>(value);
        switch (scan_type)
        {
        case ScanType::exact_value:
            return current == wanted;
        case ScanType::bigger_than:
            return current > wanted;
        case ScanType::smaller_than:
            return current < wanted;
        case ScanType::value_between:
            return std::holds_alternative<std::int64_t>(value_upper) && current >= wanted
                && current <= std::get<std::int64_t>(value_upper);
        default:
            return false;
        }
    }

    bool matches_refinement(ScanType                   scan_type,
                            ValueType                  value_type,
                            std::span<const std::byte> previous,
                            std::span<const std::byte> current)
    {
        if (previous.size() != current.size() || previous.empty())
        {
            return false;
        }
        if (scan_type == ScanType::changed)
        {
            return !std::equal(previous.begin(), previous.end(), current.begin());
        }
        if (scan_type == ScanType::unchanged)
        {
            return std::equal(previous.begin(), previous.end(), current.begin());
        }
        if (!is_numeric(value_type))
        {
            return false;
        }
        if (value_type == ValueType::float32 || value_type == ValueType::float64)
        {
            const double before = read_real(previous, value_type);
            const double after  = read_real(current, value_type);
            return scan_type == ScanType::increased ? after > before : after < before;
        }
        const std::int64_t before = read_integer(previous, value_type);
        const std::int64_t after  = read_integer(current, value_type);
        return scan_type == ScanType::increased ? after > before : after < before;
    }

    std::size_t effective_size(ValueType type, const ScanValue& value) noexcept
    {
        if (type == ValueType::string && std::holds_alternative<std::string>(value))
        {
            return std::get<std::string>(value).size();
        }
        if (type == ValueType::byte_array && std::holds_alternative<std::vector<std::byte>>(value))
        {
            return std::get<std::vector<std::byte>>(value).size();
        }
        return value_size(type);
    }

} // namespace slopkit::scan
