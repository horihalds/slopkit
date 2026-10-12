#include "structs/serializer.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slopkit::structs
{
    namespace
    {
        struct KeyValue
        {
            std::string key;
            std::string value;
        };

        [[nodiscard]] std::string quote(std::string_view value)
        {
            std::string out {"\""};
            for (const char character : value)
            {
                switch (character)
                {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    out.push_back(character);
                    break;
                }
            }
            out.push_back('"');
            return out;
        }

        [[nodiscard]] std::optional<std::string> unquote(std::string_view value)
        {
            if (value.size() < 2 || value.front() != '"' || value.back() != '"')
            {
                return std::nullopt;
            }
            std::string out;
            out.reserve(value.size() - 2);
            for (std::size_t index = 1; index + 1 < value.size(); ++index)
            {
                if (value[index] != '\\')
                {
                    out.push_back(value[index]);
                    continue;
                }
                if (index + 2 >= value.size())
                {
                    return std::nullopt;
                }
                switch (value[++index])
                {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                default:
                    return std::nullopt;
                }
            }
            return out;
        }

        [[nodiscard]] std::optional<std::uint64_t> parse_uint(std::string_view text)
        {
            int         base  = 10;
            std::size_t start = 0;
            if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
            {
                base  = 16;
                start = 2;
            }
            std::uint64_t value = 0;
            const auto    end   = text.data() + text.size();
            const auto    from  = text.data() + start;
            if (from == end)
            {
                return std::nullopt;
            }
            const auto result = std::from_chars(from, end, value, base);
            if (result.ec != std::errc {} || result.ptr != end)
            {
                return std::nullopt;
            }
            return value;
        }

        [[nodiscard]] std::expected<std::vector<KeyValue>, std::string> parse_line(std::string_view line)
        {
            std::vector<KeyValue> fields;
            std::size_t           index    = 0;
            const auto            is_space = [](char character)
            {
                return character == ' ' || character == '\t';
            };

            while (index < line.size())
            {
                while (index < line.size() && is_space(line[index]))
                {
                    ++index;
                }
                if (index >= line.size())
                {
                    break;
                }

                const std::size_t key_start = index;
                while (index < line.size() && line[index] != '=' && !is_space(line[index]))
                {
                    ++index;
                }
                const std::string key {line.substr(key_start, index - key_start)};
                if (key.empty() || index >= line.size() || line[index] != '=')
                {
                    return std::unexpected(std::format("malformed key near \"{}\"", key));
                }
                ++index;

                std::string value;
                if (index < line.size() && line[index] == '"')
                {
                    const std::size_t value_start = index++;
                    while (index < line.size() && line[index] != '"')
                    {
                        if (line[index] == '\\' && index + 1 < line.size())
                        {
                            ++index;
                        }
                        ++index;
                    }
                    if (index >= line.size())
                    {
                        return std::unexpected("unterminated quoted value");
                    }
                    ++index;
                    const auto decoded = unquote(line.substr(value_start, index - value_start));
                    if (!decoded)
                    {
                        return std::unexpected("malformed quoted value");
                    }
                    value = *decoded;
                }
                else
                {
                    const std::size_t value_start = index;
                    while (index < line.size() && !is_space(line[index]))
                    {
                        ++index;
                    }
                    value = std::string {line.substr(value_start, index - value_start)};
                }
                fields.push_back(KeyValue {key, std::move(value)});
            }
            return fields;
        }

        [[nodiscard]] const std::string* value_of(const std::vector<KeyValue>& fields, std::string_view key)
        {
            for (const KeyValue& field : fields)
            {
                if (field.key == key)
                {
                    return &field.value;
                }
            }
            return nullptr;
        }

        [[nodiscard]] std::string token_of(const FieldType& type)
        {
            switch (type.kind)
            {
            case FieldKind::integer:
                return std::format("{}{}", type.is_signed ? "i" : "u", type.width * 8);
            case FieldKind::floating:
                return type.width == 8 ? "f64" : "f32";
            case FieldKind::pointer:
                return "ptr";
            case FieldKind::array:
                return "arr";
            case FieldKind::text:
                return "text";
            case FieldKind::structure:
                return "struct";
            }
            return {};
        }

        [[nodiscard]] std::optional<FieldType> type_from_fields(const std::vector<KeyValue>& fields,
                                                                std::string_view             token)
        {
            FieldType type;
            if (token == "f32" || token == "f64")
            {
                type.kind  = FieldKind::floating;
                type.width = token == "f64" ? 8 : 4;
            }
            else if (token == "ptr")
            {
                type.kind = FieldKind::pointer;
            }
            else if (token == "arr")
            {
                type.kind = FieldKind::array;
            }
            else if (token == "text")
            {
                type.kind = FieldKind::text;
            }
            else if (token == "struct")
            {
                type.kind = FieldKind::structure;
            }
            else if ((token.size() == 2 || token.size() == 3) && (token[0] == 'i' || token[0] == 'u'))
            {
                const auto bits = parse_uint(token.substr(1));
                if (!bits || *bits == 0 || *bits % 8 != 0)
                {
                    return std::nullopt;
                }
                type.kind      = FieldKind::integer;
                type.width     = static_cast<std::size_t>(*bits / 8);
                type.is_signed = token[0] == 'i';
                if (!is_valid_width(type.width))
                {
                    return std::nullopt;
                }
            }
            else
            {
                return std::nullopt;
            }

            if (const std::string* count = value_of(fields, "count"))
            {
                const auto parsed = parse_uint(*count);
                if (!parsed || *parsed == 0)
                {
                    return std::nullopt;
                }
                type.count = static_cast<std::size_t>(*parsed);
            }
            if (const std::string* wide = value_of(fields, "wide"))
            {
                type.wide = *wide == "1";
            }
            if (const std::string* structure = value_of(fields, "struct"))
            {
                type.structure = *structure;
            }
            if (const std::string* width = value_of(fields, "width"))
            {
                const auto parsed = parse_uint(*width);
                if (!parsed || !is_valid_width(*parsed))
                {
                    return std::nullopt;
                }
                type.width = static_cast<std::size_t>(*parsed);
            }
            if (const std::string* is_signed = value_of(fields, "signed"))
            {
                type.is_signed = *is_signed == "1";
            }
            return type;
        }

        void emit_line(std::string& out, std::size_t depth, const StructNode& node)
        {
            out += std::format(
                "depth={} name={} offset=0x{:X} type={}", depth, quote(node.name), node.offset, token_of(node.type));
            const FieldType& type = node.type;
            const bool       text = type.kind == FieldKind::text;
            if (text || type.count > 1)
            {
                out += std::format(" count=0x{:X}", type.count);
            }
            if (text)
            {
                out += std::format(" wide={}", type.wide ? 1 : 0);
            }
            if (type.kind == FieldKind::array)
            {
                out += std::format(" width={} signed={}", type.width, type.is_signed ? 1 : 0);
            }
            if ((type.kind == FieldKind::pointer || type.kind == FieldKind::structure) && !type.structure.empty())
            {
                out += std::format(" struct={}", quote(type.structure));
            }
            out += std::format(" hex={}\n", node.hex ? 1 : 0);
        }
    }

    std::string to_text(const Structs& structs, std::uint64_t root)
    {
        std::string                       out;
        const std::span<const StructNode> nodes = structs.nodes();
        const std::size_t                 row   = structs.index_of(root);
        if (row >= nodes.size())
        {
            return out;
        }

        const StructNode& node = nodes[row];
        out += std::format("struct name={} expr={}\n", quote(node.name), quote(node.expression));

        const std::size_t depth = structs.depth_of(row);
        for (std::size_t child = row + 1; child < nodes.size() && structs.depth_of(child) > depth; ++child)
        {
            emit_line(out, structs.depth_of(child) - depth, nodes[child]);
        }
        return out;
    }

    std::expected<std::uint64_t, std::string> from_text(std::string_view text, Structs& structs)
    {
        std::uint64_t              root = 0;
        std::vector<std::uint64_t> by_depth;
        std::size_t                line_number = 0;

        std::size_t position = 0;
        while (position <= text.size())
        {
            const std::size_t end = text.find('\n', position);
            std::string_view  line =
                text.substr(position, end == std::string_view::npos ? std::string_view::npos : end - position);
            position = end == std::string_view::npos ? text.size() + 1 : end + 1;
            ++line_number;

            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            {
                line.remove_suffix(1);
            }
            const auto first = line.find_first_not_of(" \t");
            if (first == std::string_view::npos)
            {
                continue;
            }
            line.remove_prefix(first);

            bool is_root = false;
            if (line.starts_with("struct") && (line.size() == 6 || line[6] == ' ' || line[6] == '\t'))
            {
                is_root = true;
                line.remove_prefix(6);
            }

            const auto fields = parse_line(line);
            if (!fields)
            {
                return std::unexpected(std::format("line {}: {}", line_number, fields.error()));
            }

            if (is_root)
            {
                if (root != 0)
                {
                    return std::unexpected("a struct member holds more than one root");
                }
                const std::string* name = value_of(*fields, "name");
                const std::string* expr = value_of(*fields, "expr");
                if (name == nullptr || expr == nullptr)
                {
                    return std::unexpected("the root struct needs name and expr");
                }
                root = structs.add_struct(*name, *expr);
                by_depth.assign(1, root);
                continue;
            }

            if (root == 0)
            {
                return std::unexpected("a field appears before the root struct");
            }
            const std::string* depth  = value_of(*fields, "depth");
            const std::string* name   = value_of(*fields, "name");
            const std::string* offset = value_of(*fields, "offset");
            const std::string* type   = value_of(*fields, "type");
            if (depth == nullptr || name == nullptr || offset == nullptr || type == nullptr)
            {
                return std::unexpected(std::format("line {}: a field needs depth, name, offset and type", line_number));
            }
            const auto parsed_depth  = parse_uint(*depth);
            const auto parsed_offset = parse_uint(*offset);
            if (!parsed_depth || *parsed_depth == 0 || !parsed_offset)
            {
                return std::unexpected(std::format("line {}: malformed depth or offset", line_number));
            }
            if (*parsed_depth > by_depth.size())
            {
                return std::unexpected(std::format("line {}: depth skips a parent", line_number));
            }

            const auto field_type = type_from_fields(*fields, *type);
            if (!field_type)
            {
                return std::unexpected(std::format("line {}: unknown type \"{}\"", line_number, *type));
            }

            const std::uint64_t id = structs.add_field(by_depth[*parsed_depth - 1], *name, *parsed_offset, *field_type);
            if (id == 0)
            {
                return std::unexpected(std::format("line {}: the parent no longer exists", line_number));
            }
            if (const std::string* hex = value_of(*fields, "hex"))
            {
                structs.set_hex(id, *hex == "1");
            }

            const std::size_t index = static_cast<std::size_t>(*parsed_depth);
            by_depth.resize(index + 1);
            by_depth[index] = id;
        }

        if (root == 0)
        {
            return std::unexpected("the member holds no root struct");
        }
        return root;
    }

}
