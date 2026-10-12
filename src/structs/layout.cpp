#include "structs/layout.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace slopkit::structs
{
    namespace
    {
        [[nodiscard]] bool is_container(const StructNode& node) noexcept
        {
            return node.parent == 0 || node.type.kind == FieldKind::structure;
        }
    }

    std::vector<FieldPlacement> place(const Structs&                    structs,
                                      std::span<const std::string_view> root_names,
                                      std::span<const std::uint64_t>    root_bases,
                                      std::span<const std::uint64_t>    pointer_bases,
                                      std::size_t                       pointer_size)
    {
        const std::span<const StructNode> nodes = structs.nodes();
        std::vector<FieldPlacement>       placements(nodes.size());
        for (std::size_t row = 0; row < nodes.size(); ++row)
        {
            placements[row].id = nodes[row].id;
        }

        const std::function<void(std::size_t, std::uint64_t, std::size_t)> walk =
            [&](std::size_t row, std::uint64_t address, std::size_t depth)
        {
            if (row >= nodes.size())
            {
                return;
            }

            const StructNode& node = nodes[row];
            FieldPlacement&   here = placements[row];
            here.address           = address;
            here.size              = is_container(node) ? 0 : node.type.size(pointer_size);

            bool          expand     = is_container(node);
            std::uint64_t child_base = address;
            if (node.type.kind == FieldKind::pointer && row < pointer_bases.size() && pointer_bases[row] != 0)
            {
                expand     = true;
                child_base = pointer_bases[row];
            }
            if (!expand)
            {
                return;
            }

            std::size_t child = row + 1;
            while (child < nodes.size() && structs.depth_of(child) > depth)
            {
                if (structs.depth_of(child) == depth + 1)
                {
                    walk(child, child_base + nodes[child].offset, depth + 1);
                }
                ++child;
            }
        };

        const std::size_t roots = std::min(root_names.size(), root_bases.size());
        for (std::size_t index = 0; index < roots; ++index)
        {
            if (root_bases[index] == 0)
            {
                continue;
            }
            const StructNode* root = structs.find_struct(root_names[index]);
            if (root == nullptr)
            {
                continue;
            }
            const std::size_t row = structs.index_of(root->id);
            if (row < nodes.size())
            {
                walk(row, root_bases[index], structs.depth_of(row));
            }
        }

        return placements;
    }

    ReadPlan plan_reads(const Structs&                  structs,
                        std::span<const FieldPlacement> placements,
                        std::span<const std::uint8_t>   visible)
    {
        ReadPlan                          plan;
        const std::span<const StructNode> nodes = structs.nodes();
        for (std::size_t row = 0; row < nodes.size() && row < placements.size(); ++row)
        {
            if (row < visible.size() && visible[row] == 0)
            {
                continue;
            }
            if (placements[row].address == 0 || placements[row].size == 0)
            {
                continue;
            }
            plan.ranges.emplace_back(placements[row].address, placements[row].size);
            plan.ids.push_back(placements[row].id);
        }
        return plan;
    }

}
