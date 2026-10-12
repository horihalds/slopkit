#include "structs/export.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace slopkit::structs
{
    namespace
    {
        constexpr int kMaxNestingDepth = 32;

        [[nodiscard]] std::string integer_spelling(const FieldType& type)
        {
            return std::format("{}int{}_t", type.is_signed ? "" : "u", type.width * 8);
        }

        [[nodiscard]] std::string element_spelling(const FieldType& type)
        {
            switch (type.kind)
            {
            case FieldKind::integer:
            case FieldKind::array:
                return integer_spelling(type);
            case FieldKind::floating:
                return type.width == 8 ? "double" : "float";
            case FieldKind::pointer:
                return type.structure.empty() ? "void*" : type.structure + "*";
            case FieldKind::text:
                return type.wide ? "char16_t" : "char";
            case FieldKind::structure:
                return type.structure.empty() ? "uint8_t" : type.structure;
            }
            return "uint8_t";
        }

        // A struct whose children overlap cannot be laid out unambiguously, so
        // the export flags it instead of emitting a wrong size.
        [[nodiscard]] bool overlaps(std::vector<std::pair<std::uint64_t, std::size_t>> intervals)
        {
            std::ranges::sort(intervals);
            for (std::size_t index = 1; index < intervals.size(); ++index)
            {
                if (intervals[index].first < intervals[index - 1].second)
                {
                    return true;
                }
            }
            return false;
        }
    }

    std::string render_cpp(const Structs& structs, std::size_t pointer_size)
    {
        const std::span<const StructNode> nodes = structs.nodes();
        if (nodes.empty())
        {
            return {};
        }

        const auto child_size = [&](const StructNode& node) -> std::size_t
        {
            const std::size_t count = std::max<std::size_t>(1, node.type.count);
            if (node.type.kind == FieldKind::structure)
            {
                const StructNode* target = structs.find_struct(node.type.structure);
                if (target == nullptr)
                {
                    return 0;
                }
                return structs.struct_size(target->id, pointer_size) * count;
            }
            return node.type.size(pointer_size);
        };

        std::string out;

        for (const std::uint64_t root : structs.root_ids())
        {
            const StructNode* node = structs.find(root);
            if (node != nullptr)
            {
                out += std::format("struct {};\n", node->name);
            }
        }
        out += '\n';

        std::set<std::uint64_t>                 visited;
        std::vector<std::uint64_t>              order;
        std::function<void(std::uint64_t, int)> visit = [&](std::uint64_t id, int budget)
        {
            if (budget <= 0 || visited.contains(id))
            {
                return;
            }
            visited.insert(id);

            const std::size_t row = structs.index_of(id);
            if (row < nodes.size())
            {
                const std::size_t depth = structs.depth_of(row);
                std::size_t       end   = row + 1;
                while (end < nodes.size() && structs.depth_of(end) > depth)
                {
                    if (structs.depth_of(end) == depth + 1 && nodes[end].type.kind == FieldKind::structure)
                    {
                        if (const StructNode* target = structs.find_struct(nodes[end].type.structure);
                            target != nullptr)
                        {
                            visit(target->id, budget - 1);
                        }
                    }
                    ++end;
                }
            }
            order.push_back(id);
        };
        for (const std::uint64_t root : structs.root_ids())
        {
            visit(root, kMaxNestingDepth);
        }

        for (const std::uint64_t id : order)
        {
            const std::size_t row = structs.index_of(id);
            if (row >= nodes.size())
            {
                continue;
            }
            const StructNode& root  = nodes[row];
            const std::size_t depth = structs.depth_of(row);

            struct Child
            {
                const StructNode* node;
                std::size_t       size;
            };

            std::vector<Child>                                 children;
            std::vector<std::pair<std::uint64_t, std::size_t>> intervals;
            std::size_t                                        end = row + 1;
            while (end < nodes.size() && structs.depth_of(end) > depth)
            {
                if (structs.depth_of(end) == depth + 1)
                {
                    const std::size_t size = child_size(nodes[end]);
                    children.push_back(Child {.node = &nodes[end], .size = size});
                    if (size != 0)
                    {
                        intervals.emplace_back(nodes[end].offset, nodes[end].offset + size);
                    }
                }
                ++end;
            }
            std::ranges::sort(children,
                              {},
                              [](const Child& child)
                              {
                                  return child.node->offset;
                              });

            if (overlaps(intervals))
            {
                out += std::format("// warning: {} has fields that overlap\n", root.name);
            }
            out += std::format("struct {}\n{{\n", root.name);

            std::size_t cursor = 0;
            for (const Child& child : children)
            {
                const StructNode& field = *child.node;
                if (field.offset > cursor)
                {
                    out += std::format("    uint8_t pad_0x{:X}[{}];\n", cursor, field.offset - cursor);
                }
                const std::string name = field.name.empty() ? "field" : field.name;
                if (field.type.count > 1 && field.type.kind != FieldKind::text)
                {
                    out += std::format("    {} {}[{}];\n", element_spelling(field.type), name, field.type.count);
                }
                else if (field.type.kind == FieldKind::text)
                {
                    out += std::format("    {} {}[{}];\n", element_spelling(field.type), name, field.type.count);
                }
                else
                {
                    out += std::format("    {} {};\n", element_spelling(field.type), name);
                }
                cursor = std::max(cursor, static_cast<std::size_t>(field.offset) + child.size);
            }

            const std::size_t size = structs.struct_size(root.id, pointer_size);
            if (size > cursor)
            {
                out += std::format("    uint8_t pad_0x{:X}[{}];\n", cursor, size - cursor);
            }
            out += std::format("}}; // size 0x{:X}\n\n", size);
        }

        return out;
    }

}
