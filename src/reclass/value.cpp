#include "reclass/value.hpp"

#include <charconv>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>

#include "reclass/enum.hpp"
#include "reclass/nodes/number.hpp"
#include "reclass/project.hpp"

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

        // One code point as UTF-8, so a text node shows what the target holds.
        void append_utf8(std::string& text, std::uint32_t code)
        {
            if (code < 0x80)
            {
                text.push_back(static_cast<char>(code));
            }
            else if (code < 0x800)
            {
                text.push_back(static_cast<char>(0xC0 | (code >> 6)));
                text.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
            else if (code < 0x10000)
            {
                text.push_back(static_cast<char>(0xE0 | (code >> 12)));
                text.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                text.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
            else
            {
                text.push_back(static_cast<char>(0xF0 | (code >> 18)));
                text.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
                text.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                text.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
        }

        // A text node's bytes as text: `width` bytes per code unit, little
        // endian, stopping at the first zero unit.
        std::string decode_text(std::span<const std::byte> bytes, std::size_t width)
        {
            std::string text;
            for (std::size_t index = 0; index + width <= bytes.size(); index += width)
            {
                std::uint32_t code = 0;
                for (std::size_t part = 0; part < width; ++part)
                {
                    code |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[index + part]))
                         << (8 * part);
                }
                if (code == 0)
                {
                    break;
                }
                append_utf8(text, code);
            }
            return text;
        }

        float read_float_at(std::span<const std::byte> bytes, std::size_t offset)
        {
            std::uint32_t raw = 0;
            std::memcpy(&raw, bytes.data() + offset, sizeof(raw));
            float value {};
            std::memcpy(&value, &raw, sizeof(value));
            return value;
        }

        // A run of floats, broken into rows of `columns` (zero for one row).
        std::string format_float_run(std::span<const std::byte> bytes, std::size_t count, std::size_t columns)
        {
            std::string text;
            for (std::size_t index = 0; index < count; ++index)
            {
                if (index != 0)
                {
                    text += columns != 0 && index % columns == 0 ? " | " : ", ";
                }
                text += std::format("{:.3f}", read_float_at(bytes, index * 4));
            }
            return text;
        }

        std::string hex_run(std::span<const std::byte> bytes)
        {
            std::string text;
            text.reserve(bytes.size() * 2);
            for (const std::byte value : bytes)
            {
                text += std::format("{:02X}", std::to_integer<unsigned char>(value));
            }
            return text;
        }

        // A hex cell's dump as one run per byte, the separator carried by the
        // byte that follows it so each run stands on its own.
        std::vector<ValuePart> hex_byte_parts(std::span<const std::byte> bytes, std::size_t size)
        {
            std::vector<ValuePart> parts;
            parts.reserve(size);
            for (std::size_t index = 0; index < size; ++index)
            {
                std::string text = index == 0 ? std::string {} : std::string {" "};
                text += std::format("{:02X}", std::to_integer<unsigned char>(bytes[index]));
                parts.push_back(ValuePart {.text = std::move(text), .reading = false, .byte = index});
            }
            return parts;
        }

        float read_float(std::span<const std::byte> bytes)
        {
            float value {};
            std::memcpy(&value, bytes.data(), sizeof(value));
            return value;
        }

        double read_double(std::span<const std::byte> bytes)
        {
            double value {};
            std::memcpy(&value, bytes.data(), sizeof(value));
            return value;
        }

        // The float a hex node's bytes would read as: three decimals, or the
        // spelling of a value that is not a number at all. The reference draws
        // "#####" there, which names neither what the bytes hold nor a text the
        // user could type back.
        std::string hex_float_comment(std::span<const std::byte> bytes, bool wide)
        {
            const double value = wide ? read_double(bytes) : static_cast<double>(read_float(bytes));
            if (std::isnan(value))
            {
                return std::string {"NaN"};
            }
            if (std::isinf(value))
            {
                return std::string {value < 0.0 ? "-Inf" : "Inf"};
            }
            return std::format("{:.3f}", value);
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
            // A hex cell reads as a hex dump: every byte zero-padded and set off
            // by one space, so a hex32's four bytes are told apart at a glance.
            std::string text;
            text.reserve(size * 3);
            for (const ValuePart& part : hex_byte_parts(bytes, size))
            {
                text += part.text;
            }
            return text;
        }
        case NodeKind::pointer:
        case NodeKind::class_pointer:
        case NodeKind::class_instance_array:
            return std::format("0x{:X}", read_bytes(bytes, size));
        case NodeKind::nint:
            return std::format("{}", sign_extend(read_bytes(bytes, size), size * 8));
        case NodeKind::nuint:
            return std::format("{}", read_bytes(bytes, size));
        case NodeKind::utf8_text_ptr:
        case NodeKind::utf16_text_ptr:
        case NodeKind::utf32_text_ptr:
        case NodeKind::virtual_method_table:
        case NodeKind::virtual_method:
        case NodeKind::function_ptr:
            return std::format("0x{:X}", read_bytes(bytes, size));
        case NodeKind::vector2:
        case NodeKind::vector3:
        case NodeKind::vector4:
            return format_float_run(bytes, size / 4, 0);
        case NodeKind::matrix3x3:
            return format_float_run(bytes, 9, 3);
        case NodeKind::matrix3x4:
            return format_float_run(bytes, 12, 3);
        case NodeKind::matrix4x4:
            return format_float_run(bytes, 16, 4);
        case NodeKind::function:
            return hex_run(bytes);
        case NodeKind::bitfield:
        case NodeKind::enum_node:
        case NodeKind::utf8_text:
        case NodeKind::utf16_text:
        case NodeKind::utf32_text:
        case NodeKind::class_instance:
        case NodeKind::array:
        case NodeKind::union_value:
        case NodeKind::class_wrapper_array:
            break;
        }
        return {};
    }

    std::uint64_t read_node_value(NodeKind kind, std::span<const std::byte> bytes)
    {
        const std::size_t size = kind_size(kind);
        if (size == 0 || size > 8 || bytes.size() < size)
        {
            return 0;
        }
        return read_bytes(bytes, size);
    }

    std::string format_node_value_text(const Node& node, const Project& project, std::span<const std::byte> bytes)
    {
        switch (node.kind())
        {
        case NodeKind::bitfield:
            if (const auto* field = dynamic_cast<const BitFieldNode*>(&node); field != nullptr)
            {
                return std::format("{}", field->read(bytes));
            }
            break;
        case NodeKind::enum_node:
        {
            const std::uint64_t value = bytes.size() >= 8 ? read_bytes(bytes, 8) : read_bytes(bytes, bytes.size());
            if (const auto* enum_node = dynamic_cast<const EnumNode*>(&node); enum_node != nullptr)
            {
                if (const EnumDescription* description = project.find_enum(enum_node->enum_name);
                    description != nullptr)
                {
                    const std::string_view name = enum_value_name(*description, value);
                    if (!name.empty())
                    {
                        return std::format("{} ({})", value, name);
                    }
                }
            }
            return std::format("{}", value);
        }
        case NodeKind::utf8_text:
            return decode_text(bytes, 1);
        case NodeKind::utf16_text:
            return decode_text(bytes, 2);
        case NodeKind::utf32_text:
            return decode_text(bytes, 4);
        default:
            break;
        }
        return format_node_value_display(node.kind(), bytes, node.hex);
    }

    namespace
    {
        // The shortest spelling of the hex a kind reads: the same width-masked
        // magnitude `format_node_value` pads out, with neither zeros nor prefix.
        std::string bare_hex(NodeKind kind, std::span<const std::byte> bytes)
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
                std::uint64_t magnitude =
                    static_cast<std::uint64_t>(sign_extend(read_bytes(bytes, size), integer_bits(kind)));
                if (integer_bits(kind) < 64)
                {
                    magnitude &= (std::uint64_t {1} << integer_bits(kind)) - 1;
                }
                return std::format("{:X}", magnitude);
            }
            case NodeKind::uint8:
            case NodeKind::uint16:
            case NodeKind::uint32:
            case NodeKind::uint64:
                return std::format("{:X}", read_bytes(bytes, size));
            default:
                break;
            }
            return {};
        }

        // The display text of every kind but the hex cells that split their
        // bytes from their readings; `format_node_value_parts` owns that split.
        std::string default_node_value_display(NodeKind kind, std::span<const std::byte> bytes, bool hex)
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
                const std::string decimal = std::format("{}", sign_extend(read_bytes(bytes, size), integer_bits(kind)));
                const std::string hexes   = std::format("0x{}", bare_hex(kind, bytes));
                return hex ? std::format("{} ({})", hexes, decimal) : std::format("{} ({})", decimal, hexes);
            }
            case NodeKind::uint8:
            case NodeKind::uint16:
            case NodeKind::uint32:
            case NodeKind::uint64:
            {
                const std::string decimal = std::format("{}", read_bytes(bytes, size));
                const std::string hexes   = std::format("0x{}", bare_hex(kind, bytes));
                return hex ? std::format("{} ({})", hexes, decimal) : std::format("{} ({})", decimal, hexes);
            }
            case NodeKind::float32:
                return hex ? format_node_value(kind, bytes, true) : std::format("{:.3f}", read_float(bytes));
            case NodeKind::float64:
                return hex ? format_node_value(kind, bytes, true) : std::format("{:.3f}", read_double(bytes));
            case NodeKind::pointer:
            case NodeKind::class_pointer:
            case NodeKind::class_instance_array:
                return std::format("-> {:X}", read_bytes(bytes, size));
            case NodeKind::boolean:
            case NodeKind::character:
            case NodeKind::hex8:
            case NodeKind::hex16:
            case NodeKind::class_instance:
            case NodeKind::array:
            case NodeKind::union_value:
            case NodeKind::class_wrapper_array:
            case NodeKind::nint:
            case NodeKind::nuint:
            case NodeKind::bitfield:
            case NodeKind::enum_node:
            case NodeKind::utf8_text:
            case NodeKind::utf8_text_ptr:
            case NodeKind::utf16_text:
            case NodeKind::utf16_text_ptr:
            case NodeKind::utf32_text:
            case NodeKind::utf32_text_ptr:
            case NodeKind::vector2:
            case NodeKind::vector3:
            case NodeKind::vector4:
            case NodeKind::matrix3x3:
            case NodeKind::matrix3x4:
            case NodeKind::matrix4x4:
            case NodeKind::virtual_method_table:
            case NodeKind::virtual_method:
            case NodeKind::function:
            case NodeKind::function_ptr:
            case NodeKind::hex32:
            case NodeKind::hex64:
                break;
            }
            return format_node_value(kind, bytes, hex);
        }
    }

    std::vector<NodeReading> node_readings(NodeKind kind)
    {
        if (kind != NodeKind::hex32 && kind != NodeKind::hex64)
        {
            return {};
        }
        return {NodeReading::floating, NodeReading::integer, NodeReading::raw};
    }

    std::string format_node_reading(NodeReading reading, NodeKind kind, std::span<const std::byte> bytes)
    {
        const std::size_t size = kind_size(kind);
        if (size == 0 || bytes.size() < size || node_readings(kind).empty())
        {
            return {};
        }

        switch (reading)
        {
        case NodeReading::floating:
            return hex_float_comment(bytes, kind == NodeKind::hex64);
        case NodeReading::integer:
            return std::format("{}", sign_extend(read_bytes(bytes, size), size * 8));
        case NodeReading::raw:
            return std::format("0x{:X}", read_bytes(bytes, size));
        }
        return {};
    }

    std::string format_node_readings(NodeKind kind, std::span<const std::byte> bytes)
    {
        const std::size_t size = kind_size(kind);
        if (size == 0 || bytes.size() < size)
        {
            return {};
        }

        std::string readings;
        for (const NodeReading reading : node_readings(kind))
        {
            if (!readings.empty())
            {
                readings += ' ';
            }
            readings += std::format("({})", format_node_reading(reading, kind, bytes));
        }
        return readings;
    }

    std::vector<ValuePart> format_node_value_parts(NodeKind kind, std::span<const std::byte> bytes, bool hex)
    {
        const std::size_t size = kind_size(kind);
        if (size == 0 || bytes.size() < size)
        {
            return {};
        }

        switch (kind)
        {
        case NodeKind::hex8:
        case NodeKind::hex16:
            return hex_byte_parts(bytes, size);
        case NodeKind::hex32:
        case NodeKind::hex64:
        {
            std::vector<ValuePart> parts = hex_byte_parts(bytes, size);
            parts.push_back(ValuePart {.text = "  " + format_node_readings(kind, bytes), .reading = true});
            return parts;
        }
        default:
            break;
        }
        return {
            {default_node_value_display(kind, bytes, hex), false}
        };
    }

    std::string format_node_value_display(NodeKind kind, std::span<const std::byte> bytes, bool hex)
    {
        std::string text;
        for (const ValuePart& part : format_node_value_parts(kind, bytes, hex))
        {
            text += part.text;
        }
        return text;
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
        case NodeKind::class_instance_array:
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
        case NodeKind::nint:
        case NodeKind::nuint:
        {
            const auto literal = parse_literal(text, hex);
            if (!literal)
            {
                return std::unexpected(literal.error());
            }
            return little_endian(literal->magnitude, 8);
        }
        case NodeKind::bitfield:
        case NodeKind::enum_node:
        case NodeKind::utf8_text:
        case NodeKind::utf8_text_ptr:
        case NodeKind::utf16_text:
        case NodeKind::utf16_text_ptr:
        case NodeKind::utf32_text:
        case NodeKind::utf32_text_ptr:
        case NodeKind::vector2:
        case NodeKind::vector3:
        case NodeKind::vector4:
        case NodeKind::matrix3x3:
        case NodeKind::matrix3x4:
        case NodeKind::matrix4x4:
        case NodeKind::virtual_method_table:
        case NodeKind::virtual_method:
        case NodeKind::function:
        case NodeKind::function_ptr:
        case NodeKind::class_instance:
        case NodeKind::array:
        case NodeKind::union_value:
        case NodeKind::class_wrapper_array:
            break;
        }
        return std::unexpected(std::string {"a class instance has no value"});
    }

    std::expected<std::vector<std::byte>, std::string>
    parse_node_reading(NodeReading reading, NodeKind kind, std::string_view text, bool hex)
    {
        const std::size_t size = kind_size(kind);
        if (size == 0 || node_readings(kind).empty())
        {
            return std::unexpected(std::string {"this kind draws no readings"});
        }

        switch (reading)
        {
        case NodeReading::floating:
        {
            const std::string_view trimmed = trim(text);
            if (trimmed.empty())
            {
                return std::unexpected(std::string {"empty value"});
            }

            const std::string spelling = lower_case(trimmed);
            double            value    = 0.0;
            if (spelling == "nan")
            {
                value = std::numeric_limits<double>::quiet_NaN();
            }
            else if (spelling == "inf" || spelling == "+inf" || spelling == "infinity" || spelling == "+infinity")
            {
                value = std::numeric_limits<double>::infinity();
            }
            else if (spelling == "-inf" || spelling == "-infinity")
            {
                value = -std::numeric_limits<double>::infinity();
            }
            else
            {
                const auto [end, error] =
                    std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), value, std::chars_format::general);
                if (error == std::errc::result_out_of_range)
                {
                    return std::unexpected(out_of_range(kind));
                }
                if (error != std::errc {} || end != trimmed.data() + trimmed.size())
                {
                    return std::unexpected(std::string {"malformed floating-point number"});
                }
            }

            if (kind == NodeKind::hex32)
            {
                const float real = static_cast<float>(value);
                if (!std::isfinite(real) && std::isfinite(value))
                {
                    return std::unexpected(out_of_range(kind));
                }
                std::vector<std::byte> bytes(sizeof(real));
                std::memcpy(bytes.data(), &real, sizeof(real));
                return bytes;
            }
            std::vector<std::byte> bytes(sizeof(value));
            std::memcpy(bytes.data(), &value, sizeof(value));
            return bytes;
        }
        case NodeReading::integer:
        {
            const auto literal = parse_literal(text, hex);
            if (!literal)
            {
                return std::unexpected(literal.error());
            }

            const std::size_t bits  = size * 8;
            std::int64_t      value = 0;
            if (literal->negative)
            {
                const std::uint64_t limit = std::uint64_t {1} << (bits - 1);
                if (literal->magnitude > limit)
                {
                    return std::unexpected(out_of_range(kind));
                }
                if (bits == 64 && literal->magnitude == limit)
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
                // A hex run covers the whole width, so it is the bytes once
                // sign-extended; a run too long for the width is no value of it.
                if (bits < 64 && literal->magnitude >= (std::uint64_t {1} << bits))
                {
                    return std::unexpected(out_of_range(kind));
                }
                value = sign_extend(literal->magnitude, bits);
            }
            else
            {
                const std::uint64_t maximum = (std::uint64_t {1} << (bits - 1)) - 1;
                if (literal->magnitude > maximum)
                {
                    return std::unexpected(out_of_range(kind));
                }
                value = static_cast<std::int64_t>(literal->magnitude);
            }
            return little_endian(static_cast<std::uint64_t>(value), size);
        }
        case NodeReading::raw:
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

            const std::uint64_t mask =
                size >= 8 ? std::numeric_limits<std::uint64_t>::max() : (std::uint64_t {1} << (size * 8)) - 1;
            if (literal->magnitude > mask)
            {
                return std::unexpected(out_of_range(kind));
            }
            return little_endian(literal->magnitude, size);
        }
        }
        return std::unexpected(std::string {"this kind draws no readings"});
    }

}
