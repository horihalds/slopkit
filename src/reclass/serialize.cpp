#include "reclass/serialize.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include "reclass/edit.hpp"
#include "reclass/enum.hpp"
#include "reclass/nodes/container.hpp"
#include "reclass/nodes/node.hpp"
#include "reclass/nodes/number.hpp"
#include "reclass/nodes/text.hpp"

namespace slopkit::reclass
{
    namespace
    {
        constexpr std::string_view kIndexMember   = "reclass/index.txt";
        constexpr std::string_view kClassesPrefix = "reclass/classes/";
        constexpr std::string_view kEnumsMember   = "reclass/enums.txt";

        // The version a class-project member carries on its own first line. A
        // member without it was written by a build whose node grammar did not
        // carry the per-kind parameters, so it is read as padding rather than
        // misread as the current grammar.
        constexpr std::string_view kProjectVersion = "v2";

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

        bool valid_stem_character(char character)
        {
            return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9') || character == '_' || character == '-' || character == '.';
        }

        std::string member_name(std::string_view name, std::span<const std::string> taken)
        {
            std::string stem;
            for (const char character : trim(name))
            {
                stem += valid_stem_character(character) ? character : '_';
            }
            if (stem.empty())
            {
                stem = "class";
            }

            std::string candidate = std::format("{}{}.txt", kClassesPrefix, stem);
            for (std::size_t suffix = 2; std::ranges::find(taken, candidate) != taken.end(); ++suffix)
            {
                candidate = std::format("{}{}_{}.txt", kClassesPrefix, stem, suffix);
            }
            return candidate;
        }

        // Reads the `key="quoted value"` and `key=bare` attributes the member
        // text is built from.
        template<typename Visitor>
        std::expected<void, std::string>
        parse_attributes(std::string_view text, std::string_view context, Visitor&& visit)
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
                    ++position;
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

        template<typename Integer>
        std::optional<Integer> parse_uint(std::string_view text)
        {
            Integer parsed          = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (error != std::errc {} || end != text.data() + text.size())
            {
                return std::nullopt;
            }
            return parsed;
        }

        std::vector<std::string_view> split_lines(std::string_view text)
        {
            std::vector<std::string_view> lines;
            std::size_t                   position = 0;
            while (position <= text.size())
            {
                const auto             newline = text.find('\n', position);
                const std::string_view raw     = text.substr(
                    position, newline == std::string_view::npos ? std::string_view::npos : newline - position);
                const std::string_view line = trim(raw);
                if (!line.empty())
                {
                    lines.push_back(line);
                }
                if (newline == std::string_view::npos)
                {
                    break;
                }
                position = newline + 1;
            }
            return lines;
        }

        const Member* find_member(std::span<const Member> members, std::string_view name)
        {
            const auto found = std::ranges::find(members, name, &Member::name);
            return found == members.end() ? nullptr : &*found;
        }

        unsigned flags_of(const Node& node) noexcept
        {
            return (node.hex ? 1U : 0U) | (node.hidden ? 2U : 0U);
        }

        void assign_ids(Project& project, std::vector<NodePtr>& nodes)
        {
            for (NodePtr& node : nodes)
            {
                node->id = project.next_node_id();
                if (auto* container = dynamic_cast<ContainerNode*>(node.get()); container != nullptr)
                {
                    assign_ids(project, container->children_mutable());
                }
            }
        }

        // An array line whose element lines were lost still holds one default
        // element, the shape make_node builds, so a container never comes back
        // empty.
        void ensure_array_elements(std::vector<NodePtr>& nodes)
        {
            for (NodePtr& node : nodes)
            {
                if (auto* array = dynamic_cast<ArrayNode*>(node.get());
                    array != nullptr && array->children_mutable().empty())
                {
                    array->children_mutable().push_back(make_node(NodeKind::int8));
                }
                if (auto* container = dynamic_cast<ContainerNode*>(node.get()); container != nullptr)
                {
                    ensure_array_elements(container->children_mutable());
                }
            }
        }

        void append_node(std::string& text, const Node& node, std::size_t depth)
        {
            const NodeKind kind = node.kind();
            text += std::format("node depth={} kind={} flags={} name=\"{}\" comment=\"{}\"",
                                depth,
                                type_token(kind),
                                flags_of(node),
                                escape(node.name),
                                escape(node.comment));
            if (is_class_kind(kind))
            {
                text += std::format(" class=\"{}\"", escape(node.class_name));
            }
            if (const auto* bitfield = dynamic_cast<const BitFieldNode*>(&node); bitfield != nullptr)
            {
                text += std::format(" bits={} signed={}", bitfield->bits, bitfield->signed_bits ? 1 : 0);
            }
            else if (const auto* enum_node = dynamic_cast<const EnumNode*>(&node); enum_node != nullptr)
            {
                text += std::format(" enum=\"{}\" size={}", escape(enum_node->enum_name), enum_node->size_bytes);
            }
            else if (const auto* text8 = dynamic_cast<const UTF8TextNode*>(&node); text8 != nullptr)
            {
                text += std::format(" length={}", text8->length);
            }
            else if (const auto* text16 = dynamic_cast<const UTF16TextNode*>(&node); text16 != nullptr)
            {
                text += std::format(" length={}", text16->length);
            }
            else if (const auto* text32 = dynamic_cast<const UTF32TextNode*>(&node); text32 != nullptr)
            {
                text += std::format(" length={}", text32->length);
            }
            else if (const auto* instance_array = dynamic_cast<const ClassInstanceArrayNode*>(&node);
                     instance_array != nullptr)
            {
                text += std::format(" count={}", instance_array->count);
            }
            else if (const auto* wrapper_array = dynamic_cast<const ClassWrapperArrayNode*>(&node);
                     wrapper_array != nullptr)
            {
                text += std::format(" count={}", wrapper_array->count);
            }
            text += '\n';

            for (const NodePtr& child : node.children())
            {
                append_node(text, *child, depth + 1);
            }
        }

        std::expected<std::pair<NodePtr, std::size_t>, std::string> parse_node_line(std::string_view text,
                                                                                    std::string_view member)
        {
            std::optional<NodeKind>    kind;
            std::size_t                depth      = 0;
            bool                       seen_depth = false;
            bool                       seen_flags = false;
            bool                       seen_class = false;
            bool                       hex        = false;
            bool                       hidden     = false;
            std::string                name;
            std::string                comment;
            std::string                class_name;
            std::optional<std::size_t> bits;
            std::optional<bool>        signed_bits;
            std::optional<std::string> enum_name;
            std::optional<std::size_t> enum_size;
            std::optional<std::size_t> text_length;
            std::optional<std::size_t> count;

            const auto visit = [&](const std::string& key, std::string value) -> std::expected<void, std::string>
            {
                if (key == "kind")
                {
                    kind = kind_from_token(value);
                    if (!kind)
                    {
                        return std::unexpected(std::format("{}: unknown node kind '{}'", member, value));
                    }
                }
                else if (key == "depth")
                {
                    const auto parsed = parse_uint<std::size_t>(value);
                    if (!parsed)
                    {
                        return std::unexpected(std::format("{}: invalid depth '{}'", member, value));
                    }
                    depth      = *parsed;
                    seen_depth = true;
                }
                else if (key == "flags")
                {
                    const auto parsed = parse_uint<unsigned>(value);
                    if (!parsed)
                    {
                        return std::unexpected(std::format("{}: invalid flags '{}'", member, value));
                    }
                    hex        = (*parsed & 1U) != 0;
                    hidden     = (*parsed & 2U) != 0;
                    seen_flags = true;
                }
                else if (key == "name")
                {
                    name = std::move(value);
                }
                else if (key == "comment")
                {
                    comment = std::move(value);
                }
                else if (key == "class")
                {
                    class_name = std::move(value);
                    seen_class = true;
                }
                else if (key == "bits")
                {
                    const auto parsed = parse_uint<std::size_t>(value);
                    if (!parsed)
                    {
                        return std::unexpected(std::format("{}: invalid bit count '{}'", member, value));
                    }
                    bits = *parsed;
                }
                else if (key == "signed")
                {
                    const auto flag = parse_bool(value);
                    if (!flag)
                    {
                        return std::unexpected(std::format("{}: invalid boolean '{}'", member, value));
                    }
                    signed_bits = *flag;
                }
                else if (key == "enum")
                {
                    enum_name = std::move(value);
                }
                else if (key == "size")
                {
                    const auto parsed = parse_uint<std::size_t>(value);
                    if (!parsed)
                    {
                        return std::unexpected(std::format("{}: invalid size '{}'", member, value));
                    }
                    enum_size = *parsed;
                }
                else if (key == "length")
                {
                    const auto parsed = parse_uint<std::size_t>(value);
                    if (!parsed)
                    {
                        return std::unexpected(std::format("{}: invalid length '{}'", member, value));
                    }
                    text_length = *parsed;
                }
                else if (key == "count")
                {
                    const auto parsed = parse_uint<std::size_t>(value);
                    if (!parsed)
                    {
                        return std::unexpected(std::format("{}: invalid count '{}'", member, value));
                    }
                    count = *parsed;
                }
                else
                {
                    return std::unexpected(std::format("{}: unknown key '{}'", member, key));
                }
                return {};
            };

            auto parsed = parse_attributes(text, member, visit);
            if (!parsed)
            {
                return std::unexpected(parsed.error());
            }
            if (!kind)
            {
                return std::unexpected(std::format("{}: node without 'kind'", member));
            }
            if (!seen_depth)
            {
                return std::unexpected(std::format("{}: node without 'depth'", member));
            }
            if (!seen_flags)
            {
                return std::unexpected(std::format("{}: node without 'flags'", member));
            }

            NodePtr node     = make_node(*kind);
            node->name       = std::move(name);
            node->comment    = std::move(comment);
            node->class_name = std::move(class_name);
            node->hex        = hex;
            node->hidden     = hidden;

            const NodeKind created = node->kind();
            if (is_class_kind(created) && !seen_class)
            {
                return std::unexpected(std::format("{}: {} node without 'class'", member, type_token(created)));
            }

            // make_node seeds a fresh array with one element; the element lines
            // that follow replace it.
            if (auto* container = dynamic_cast<ContainerNode*>(node.get()); container != nullptr)
            {
                container->children_mutable().clear();
            }

            if (created == NodeKind::bitfield)
            {
                if (!bits)
                {
                    return std::unexpected(std::format("{}: bitfield without 'bits'", member));
                }
                auto* bitfield        = static_cast<BitFieldNode*>(node.get());
                bitfield->bits        = *bits;
                bitfield->signed_bits = signed_bits.value_or(false);
            }
            else if (created == NodeKind::enum_node)
            {
                if (!enum_name || !enum_size)
                {
                    return std::unexpected(std::format("{}: enum node without 'enum' and 'size'", member));
                }
                auto* enum_node       = static_cast<EnumNode*>(node.get());
                enum_node->enum_name  = std::move(*enum_name);
                enum_node->size_bytes = *enum_size;
            }
            else if (created == NodeKind::utf8_text)
            {
                if (!text_length)
                {
                    return std::unexpected(std::format("{}: text node without 'length'", member));
                }
                static_cast<UTF8TextNode*>(node.get())->length = *text_length;
            }
            else if (created == NodeKind::utf16_text)
            {
                if (!text_length)
                {
                    return std::unexpected(std::format("{}: text node without 'length'", member));
                }
                static_cast<UTF16TextNode*>(node.get())->length = *text_length;
            }
            else if (created == NodeKind::utf32_text)
            {
                if (!text_length)
                {
                    return std::unexpected(std::format("{}: text node without 'length'", member));
                }
                static_cast<UTF32TextNode*>(node.get())->length = *text_length;
            }
            else if (created == NodeKind::class_instance_array)
            {
                if (!count)
                {
                    return std::unexpected(std::format("{}: class instance array without 'count'", member));
                }
                static_cast<ClassInstanceArrayNode*>(node.get())->count = *count;
            }
            else if (created == NodeKind::class_wrapper_array)
            {
                if (!count)
                {
                    return std::unexpected(std::format("{}: class array without 'count'", member));
                }
                static_cast<ClassWrapperArrayNode*>(node.get())->count = *count;
            }

            return std::make_pair(std::move(node), depth);
        }

        std::expected<Class, std::string> parse_class(std::string_view text, std::string_view member)
        {
            const std::vector<std::string_view> lines = split_lines(text);

            Class       type;
            bool        seen_name = false;
            std::size_t position  = 0;
            bool        versioned = false;
            if (position < lines.size() && lines[position] == kProjectVersion)
            {
                versioned = true;
                ++position;
            }

            if (position >= lines.size() || lines[position].starts_with("node "))
            {
                return std::unexpected(std::format("{}: missing 'name'", member));
            }

            const auto visit = [&](const std::string& key, std::string value) -> std::expected<void, std::string>
            {
                if (key == "name")
                {
                    type.name = std::move(value);
                    seen_name = true;
                }
                else if (key == "address")
                {
                    type.address_expression = std::move(value);
                }
                else
                {
                    return std::unexpected(std::format("{}: unknown key '{}'", member, key));
                }
                return {};
            };
            auto header = parse_attributes(lines[position], member, visit);
            if (!header)
            {
                return std::unexpected(header.error());
            }
            ++position;

            if (!seen_name)
            {
                return std::unexpected(std::format("{}: missing 'name'", member));
            }
            if (type.name.empty())
            {
                return std::unexpected(std::format("{}: empty class name", member));
            }

            // A pre-v2 payload still names the class, but its node grammar does
            // not carry the per-kind parameters, so the class is rebuilt as
            // padding instead of read into a shape it never had.
            if (!versioned)
            {
                type.nodes = pad_nodes(kDefaultClassBytes);
                return type;
            }

            std::vector<std::pair<NodePtr, std::size_t>> pending;
            for (; position < lines.size(); ++position)
            {
                if (!lines[position].starts_with("node "))
                {
                    return std::unexpected(
                        std::format("{}: expected a node line, found '{}'", member, lines[position]));
                }
                auto node = parse_node_line(lines[position].substr(5), member);
                if (!node)
                {
                    return std::unexpected(node.error());
                }
                pending.push_back(std::move(*node));
            }

            // The depth attribute nests container children: a node's children
            // are the following lines one level deeper.
            std::vector<std::pair<std::size_t, ContainerNode*>> open;
            for (auto& [node, depth] : pending)
            {
                while (!open.empty() && open.back().first >= depth)
                {
                    open.pop_back();
                }
                const std::size_t expected = open.empty() ? 0 : open.back().first + 1;
                if (depth != expected)
                {
                    return std::unexpected(
                        std::format("{}: node depth {} where {} was expected", member, depth, expected));
                }

                std::vector<NodePtr>* owner = open.empty() ? &type.nodes : &open.back().second->children_mutable();
                owner->push_back(std::move(node));
                if (auto* container = dynamic_cast<ContainerNode*>(owner->back().get()); container != nullptr)
                {
                    open.emplace_back(depth, container);
                }
            }

            ensure_array_elements(type.nodes);
            return type;
        }

        std::expected<void, std::string> parse_enums(Project& project, std::string_view text, std::string_view member)
        {
            EnumDescription* current = nullptr;
            for (const std::string_view line : split_lines(text))
            {
                if (line.starts_with("enum "))
                {
                    std::string                name;
                    std::optional<std::size_t> size;
                    const auto                 visit = [&](const std::string& key,
                                                           std::string        value) -> std::expected<void, std::string>
                    {
                        if (key == "name")
                        {
                            name = std::move(value);
                        }
                        else if (key == "size")
                        {
                            const auto parsed = parse_uint<std::size_t>(value);
                            if (!parsed)
                            {
                                return std::unexpected(std::format("{}: invalid size '{}'", member, value));
                            }
                            size = *parsed;
                        }
                        else
                        {
                            return std::unexpected(std::format("{}: unknown key '{}'", member, key));
                        }
                        return {};
                    };
                    auto parsed = parse_attributes(line.substr(5), member, visit);
                    if (!parsed)
                    {
                        return std::unexpected(parsed.error());
                    }
                    if (!size || !is_enum_size(*size))
                    {
                        return std::unexpected(std::format("{}: enum '{}' without a valid size", member, name));
                    }
                    current = project.add_enum(std::move(name), *size);
                    if (current == nullptr)
                    {
                        return std::unexpected(std::format("{}: invalid enum name", member));
                    }
                }
                else if (line.starts_with("value "))
                {
                    if (current == nullptr)
                    {
                        return std::unexpected(std::format("{}: value before any enum", member));
                    }

                    std::string   name;
                    std::string   comment;
                    std::uint64_t value      = 0;
                    bool          seen_value = false;
                    const auto    visit      = [&](const std::string& key,
                                                   std::string        text_value) -> std::expected<void, std::string>
                    {
                        if (key == "name")
                        {
                            name = std::move(text_value);
                        }
                        else if (key == "value")
                        {
                            const auto parsed = parse_uint<std::uint64_t>(text_value);
                            if (!parsed)
                            {
                                return std::unexpected(std::format("{}: invalid value '{}'", member, text_value));
                            }
                            value      = *parsed;
                            seen_value = true;
                        }
                        else if (key == "comment")
                        {
                            comment = std::move(text_value);
                        }
                        else
                        {
                            return std::unexpected(std::format("{}: unknown key '{}'", member, key));
                        }
                        return {};
                    };
                    auto parsed = parse_attributes(line.substr(6), member, visit);
                    if (!parsed)
                    {
                        return std::unexpected(parsed.error());
                    }
                    if (!seen_value)
                    {
                        return std::unexpected(std::format("{}: value without 'value'", member));
                    }
                    current->values.push_back(
                        EnumValue {.name = std::move(name), .value = value, .comment = std::move(comment)});
                }
                else
                {
                    return std::unexpected(std::format("{}: expected an enum or value line, found '{}'", member, line));
                }
            }
            return {};
        }

        std::expected<void, std::string> add_class(Project& project, Class type, std::string_view member)
        {
            if (project.find_class(type.name) != nullptr)
            {
                return std::unexpected(std::format("{}: duplicate class name '{}'", member, type.name));
            }
            if (project.add_class(type.name, std::move(type.address_expression)) == 0)
            {
                return std::unexpected(std::format("{}: invalid class name '{}'", member, type.name));
            }
            assign_ids(project, type.nodes);
            project.classes().back().nodes = std::move(type.nodes);
            return {};
        }
    }

    std::vector<Member> to_members(const Project& project)
    {
        std::vector<Member> members;
        if (project.classes().empty() && project.enums().empty())
        {
            return members;
        }

        std::vector<std::string> taken;
        std::string              index = std::format("{}\n", kProjectVersion);
        std::vector<Member>      classes;
        taken.reserve(project.classes().size());
        classes.reserve(project.classes().size());

        for (const Class& type : project.classes())
        {
            const std::string name = member_name(type.name, taken);
            taken.push_back(name);
            index += name;
            index += '\n';

            std::string text = std::format("{}\nname=\"{}\" address=\"{}\"\n",
                                           kProjectVersion,
                                           escape(type.name),
                                           escape(type.address_expression));
            for (const NodePtr& node : type.nodes)
            {
                append_node(text, *node, 0);
            }
            classes.push_back(Member {.name = name, .text = std::move(text)});
        }

        members.push_back(Member {.name = std::string(kIndexMember), .text = std::move(index)});
        members.insert(members.end(), std::make_move_iterator(classes.begin()), std::make_move_iterator(classes.end()));

        if (!project.enums().empty())
        {
            std::string text;
            for (const EnumDescription& description : project.enums())
            {
                text += std::format("enum name=\"{}\" size={}\n", escape(description.name), description.size);
                for (const EnumValue& value : description.values)
                {
                    text += std::format("value name=\"{}\" value={} comment=\"{}\"\n",
                                        escape(value.name),
                                        value.value,
                                        escape(value.comment));
                }
            }
            members.push_back(Member {.name = std::string(kEnumsMember), .text = std::move(text)});
        }
        return members;
    }

    std::expected<Project, std::string> from_members(std::span<const Member> members)
    {
        Project project;

        const Member* index = find_member(members, kIndexMember);
        if (index == nullptr)
        {
            for (const Member& member : members)
            {
                if (member.name.starts_with(kClassesPrefix) || member.name == kEnumsMember)
                {
                    return std::unexpected(std::format("{}: missing", kIndexMember));
                }
            }
            return project;
        }

        const std::vector<std::string_view> entries = split_lines(index->text);
        std::vector<std::string>            listed;
        std::size_t                         first = 0;
        if (!entries.empty() && entries.front() == kProjectVersion)
        {
            first = 1;
        }
        for (std::size_t position = first; position < entries.size(); ++position)
        {
            const std::string name {entries[position]};
            if (!name.starts_with(kClassesPrefix))
            {
                return std::unexpected(std::format("{}: unexpected entry '{}'", kIndexMember, name));
            }
            if (std::ranges::find(listed, name) != listed.end())
            {
                return std::unexpected(std::format("{}: duplicate entry '{}'", kIndexMember, name));
            }

            const Member* body = find_member(members, name);
            if (body == nullptr)
            {
                return std::unexpected(std::format("{}: missing class '{}'", kIndexMember, name));
            }

            auto type = parse_class(body->text, name);
            if (!type)
            {
                return std::unexpected(type.error());
            }

            auto added = add_class(project, std::move(*type), name);
            if (!added)
            {
                return std::unexpected(added.error());
            }
            listed.push_back(name);
        }

        for (const Member& member : members)
        {
            if (member.name.starts_with(kClassesPrefix) && std::ranges::find(listed, member.name) == listed.end())
            {
                return std::unexpected(std::format("{}: not listed in {}", member.name, kIndexMember));
            }
        }

        if (const Member* enums = find_member(members, kEnumsMember); enums != nullptr)
        {
            auto parsed = parse_enums(project, enums->text, enums->name);
            if (!parsed)
            {
                return std::unexpected(parsed.error());
            }
        }
        return project;
    }

}
