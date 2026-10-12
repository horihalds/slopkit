#include "structs/definition.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace slopkit::structs
{
    namespace
    {
        // Embedded-struct recursion is capped rather than trusted: a definition
        // that nests a struct into itself would otherwise never terminate.
        constexpr int kMaxNestingDepth = 32;

        [[nodiscard]] std::size_t align_up(std::size_t value, std::size_t alignment) noexcept
        {
            const std::size_t step = alignment == 0 ? 1 : alignment;
            return (value + step - 1) / step * step;
        }
    }

    std::uint64_t Structs::add_struct(std::string name, std::string expression)
    {
        StructNode node;
        node.id         = next_id_++;
        node.parent     = 0;
        node.name       = std::move(name);
        node.expression = std::move(expression);
        nodes_.push_back(std::move(node));
        return nodes_.back().id;
    }

    std::uint64_t Structs::add_field(std::uint64_t parent, std::string name, std::uint64_t offset, FieldType type)
    {
        const std::size_t parent_row = index_of(parent);
        if (parent_row >= nodes_.size())
        {
            return 0;
        }

        const std::size_t depth = depth_of(parent_row);
        std::size_t       end   = parent_row + 1;
        while (end < nodes_.size() && depth_of(end) > depth)
        {
            ++end;
        }

        StructNode node;
        node.id     = next_id_++;
        node.parent = parent;
        node.name   = std::move(name);
        node.offset = offset;
        node.type   = std::move(type);

        const std::uint64_t id = node.id;
        nodes_.insert(nodes_.begin() + static_cast<std::ptrdiff_t>(end), std::move(node));
        return id;
    }

    bool Structs::rename(std::uint64_t id, std::string name)
    {
        const std::size_t row = index_of(id);
        if (row >= nodes_.size())
        {
            return false;
        }
        nodes_[row].name = std::move(name);
        return true;
    }

    bool Structs::set_offset(std::uint64_t id, std::uint64_t offset)
    {
        const std::size_t row = index_of(id);
        if (row >= nodes_.size())
        {
            return false;
        }
        nodes_[row].offset = offset;
        return true;
    }

    bool Structs::set_type(std::uint64_t id, FieldType type)
    {
        const std::size_t row = index_of(id);
        if (row >= nodes_.size())
        {
            return false;
        }
        nodes_[row].type = std::move(type);
        return true;
    }

    bool Structs::set_expression(std::uint64_t id, std::string expression)
    {
        const std::size_t row = index_of(id);
        if (row >= nodes_.size())
        {
            return false;
        }
        nodes_[row].expression = std::move(expression);
        return true;
    }

    bool Structs::set_hex(std::uint64_t id, bool hex)
    {
        const std::size_t row = index_of(id);
        if (row >= nodes_.size())
        {
            return false;
        }
        nodes_[row].hex = hex;
        return true;
    }

    bool Structs::remove(std::uint64_t id)
    {
        const std::size_t row = index_of(id);
        if (row >= nodes_.size())
        {
            return false;
        }

        const std::size_t depth = depth_of(row);
        std::size_t       end   = row + 1;
        while (end < nodes_.size() && depth_of(end) > depth)
        {
            ++end;
        }
        nodes_.erase(nodes_.begin() + static_cast<std::ptrdiff_t>(row),
                     nodes_.begin() + static_cast<std::ptrdiff_t>(end));
        return true;
    }

    void Structs::clear()
    {
        nodes_.clear();
        next_id_ = 1;
    }

    std::span<const StructNode> Structs::nodes() const noexcept
    {
        return nodes_;
    }

    bool Structs::empty() const noexcept
    {
        return nodes_.empty();
    }

    std::size_t Structs::size() const noexcept
    {
        return nodes_.size();
    }

    std::size_t Structs::depth_of(std::size_t row) const noexcept
    {
        if (row >= nodes_.size())
        {
            return 0;
        }

        std::size_t   depth  = 0;
        std::uint64_t parent = nodes_[row].parent;
        std::size_t   guard  = nodes_.size() + 1;
        while (parent != 0 && guard-- > 0)
        {
            const std::size_t parent_row = index_of(parent);
            if (parent_row >= nodes_.size())
            {
                break;
            }
            ++depth;
            parent = nodes_[parent_row].parent;
        }
        return depth;
    }

    const StructNode* Structs::find(std::uint64_t id) const noexcept
    {
        for (const StructNode& node : nodes_)
        {
            if (node.id == id)
            {
                return &node;
            }
        }
        return nullptr;
    }

    std::size_t Structs::index_of(std::uint64_t id) const noexcept
    {
        for (std::size_t row = 0; row < nodes_.size(); ++row)
        {
            if (nodes_[row].id == id)
            {
                return row;
            }
        }
        return nodes_.size();
    }

    std::uint64_t Structs::root_of(std::size_t row) const noexcept
    {
        if (row >= nodes_.size())
        {
            return 0;
        }

        std::size_t guard = nodes_.size() + 1;
        while (nodes_[row].parent != 0 && guard-- > 0)
        {
            const std::size_t parent_row = index_of(nodes_[row].parent);
            if (parent_row >= nodes_.size())
            {
                break;
            }
            row = parent_row;
        }
        return nodes_[row].id;
    }

    std::vector<std::uint64_t> Structs::root_ids() const
    {
        std::vector<std::uint64_t> ids;
        for (const StructNode& node : nodes_)
        {
            if (node.parent == 0)
            {
                ids.push_back(node.id);
            }
        }
        return ids;
    }

    const StructNode* Structs::find_struct(std::string_view name) const noexcept
    {
        if (name.empty())
        {
            return nullptr;
        }
        for (const StructNode& node : nodes_)
        {
            if (node.parent == 0 && node.name == name)
            {
                return &node;
            }
        }
        return nullptr;
    }

    std::size_t Structs::struct_size(std::uint64_t id, std::size_t pointer_size) const
    {
        const std::size_t row = index_of(id);
        if (row >= nodes_.size())
        {
            return 0;
        }
        return struct_size_at(row, pointer_size, kMaxNestingDepth);
    }

    std::size_t Structs::struct_size_at(std::size_t row, std::size_t pointer_size, int budget) const
    {
        if (row >= nodes_.size() || budget <= 0)
        {
            return 0;
        }

        const std::size_t depth = depth_of(row);
        std::size_t       end   = row + 1;
        while (end < nodes_.size() && depth_of(end) > depth)
        {
            ++end;
        }

        std::size_t extent = 0;
        for (std::size_t child = row + 1; child < end; ++child)
        {
            if (depth_of(child) != depth + 1)
            {
                continue;
            }

            const StructNode& node = nodes_[child];
            std::size_t       end  = static_cast<std::size_t>(node.offset);
            if (node.type.kind == FieldKind::structure)
            {
                const StructNode* target = find_struct(node.type.structure);
                const std::size_t element =
                    target == nullptr ? 0 : struct_size_at(index_of(target->id), pointer_size, budget - 1);
                end += element * std::max<std::size_t>(1, node.type.count);
            }
            else
            {
                end += node.type.size(pointer_size);
            }
            extent = std::max(extent, end);
        }

        return align_up(extent, pointer_size);
    }

    std::vector<std::string> Structs::struct_names() const
    {
        std::vector<std::string> names;
        for (const StructNode& node : nodes_)
        {
            if (node.parent == 0)
            {
                names.push_back(node.name);
            }
        }
        return names;
    }

}
