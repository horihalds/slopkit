#include "reclass/serialize.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <optional>
#include <utility>

namespace slopkit::reclass
{
    namespace
    {
        constexpr std::string_view kIndexMember   = "reclass/index.txt";
        constexpr std::string_view kClassesPrefix = "reclass/classes/";

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

        const Member* find_member(std::span<const Member> members, std::string_view name)
        {
            const auto found = std::ranges::find(members, name, &Member::name);
            return found == members.end() ? nullptr : &*found;
        }

        std::expected<Class, std::string> parse_class(std::string_view text, std::string_view member)
        {
            Class       type;
            bool        seen_name = false;
            bool        seen_body = false;
            std::size_t position  = 0;

            while (position <= text.size())
            {
                const auto             newline = text.find('\n', position);
                const std::string_view raw     = text.substr(
                    position, newline == std::string_view::npos ? std::string_view::npos : newline - position);
                const std::string_view line = trim(raw);

                if (!line.empty() && !seen_body)
                {
                    const auto visit = [&](const std::string& key,
                                           std::string        value) -> std::expected<void, std::string>
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

                    auto parsed = parse_attributes(line, member, visit);
                    if (!parsed)
                    {
                        return std::unexpected(parsed.error());
                    }
                    seen_body = true;
                }
                else if (!line.empty())
                {
                    if (!line.starts_with("node "))
                    {
                        return std::unexpected(std::format("{}: expected a node line, found '{}'", member, line));
                    }

                    std::optional<NodeKind> kind;
                    std::string             node_name;
                    std::string             node_comment;
                    std::string             node_class;
                    bool                    hex      = false;
                    bool                    seen_hex = false;
                    const auto              visit    = [&](const std::string& key,
                                                           std::string        value) -> std::expected<void, std::string>
                    {
                        if (key == "kind")
                        {
                            kind = kind_from_token(value);
                            if (!kind)
                            {
                                return std::unexpected(std::format("{}: unknown node kind '{}'", member, value));
                            }
                        }
                        else if (key == "name")
                        {
                            node_name = std::move(value);
                        }
                        else if (key == "comment")
                        {
                            node_comment = std::move(value);
                        }
                        else if (key == "class")
                        {
                            node_class = std::move(value);
                        }
                        else if (key == "hex")
                        {
                            const auto flag = parse_bool(value);
                            if (!flag)
                            {
                                return std::unexpected(std::format("{}: invalid boolean '{}'", member, value));
                            }
                            hex      = *flag;
                            seen_hex = true;
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
                    if (!kind)
                    {
                        return std::unexpected(std::format("{}: node without 'kind'", member));
                    }
                    if (!seen_hex)
                    {
                        return std::unexpected(std::format("{}: node without 'hex'", member));
                    }

                    NodePtr created     = make_node(*kind);
                    created->name       = std::move(node_name);
                    created->comment    = std::move(node_comment);
                    created->class_name = std::move(node_class);
                    created->hex        = hex;
                    type.nodes.push_back(std::move(created));
                }

                if (newline == std::string_view::npos)
                {
                    break;
                }
                position = newline + 1;
            }

            if (!seen_name)
            {
                return std::unexpected(std::format("{}: missing 'name'", member));
            }
            if (type.name.empty())
            {
                return std::unexpected(std::format("{}: empty class name", member));
            }
            return type;
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
            for (NodePtr& node : type.nodes)
            {
                node->id = project.next_node_id();
            }
            project.classes().back().nodes = std::move(type.nodes);
            return {};
        }
    }

    std::vector<Member> to_members(const Project& project)
    {
        std::vector<Member> members;
        if (project.classes().empty())
        {
            return members;
        }

        std::vector<std::string> taken;
        std::string              index;
        std::vector<Member>      classes;
        taken.reserve(project.classes().size());
        classes.reserve(project.classes().size());

        for (const Class& type : project.classes())
        {
            const std::string name = member_name(type.name, taken);
            taken.push_back(name);
            index += name;
            index += '\n';

            std::string text =
                std::format("name=\"{}\" address=\"{}\"\n", escape(type.name), escape(type.address_expression));
            for (const NodePtr& node : type.nodes)
            {
                text += std::format("node kind={} name=\"{}\" hex={} comment=\"{}\"",
                                    type_token(node->kind()),
                                    escape(node->name),
                                    node->hex ? 1 : 0,
                                    escape(node->comment));
                if (is_class_kind(node->kind()))
                {
                    text += std::format(" class=\"{}\"", escape(node->class_name));
                }
                text += '\n';
            }
            classes.push_back(Member {.name = name, .text = std::move(text)});
        }

        members.push_back(Member {.name = std::string(kIndexMember), .text = std::move(index)});
        members.insert(members.end(), std::make_move_iterator(classes.begin()), std::make_move_iterator(classes.end()));
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
                if (member.name.starts_with(kClassesPrefix))
                {
                    return std::unexpected(std::format("{}: missing", kIndexMember));
                }
            }
            return project;
        }

        std::vector<std::string> listed;
        std::size_t              position = 0;
        while (position <= index->text.size())
        {
            const auto             newline = index->text.find('\n', position);
            const std::string_view raw     = std::string_view {index->text}.substr(
                position, newline == std::string::npos ? std::string::npos : newline - position);
            const std::string_view line = trim(raw);

            if (!line.empty())
            {
                const std::string name {line};
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

            if (newline == std::string::npos)
            {
                break;
            }
            position = newline + 1;
        }

        for (const Member& member : members)
        {
            if (member.name.starts_with(kClassesPrefix) && std::ranges::find(listed, member.name) == listed.end())
            {
                return std::unexpected(std::format("{}: not listed in {}", member.name, kIndexMember));
            }
        }
        return project;
    }

}
