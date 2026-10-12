#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "structs/types.hpp"

namespace slopkit::structs
{
    struct StructNode
    {
        std::uint64_t id {};
        std::uint64_t parent {};
        std::string   name;
        std::uint64_t offset {};
        FieldType     type;
        std::string   expression;
        bool          hex {false};

        friend bool operator==(const StructNode&, const StructNode&) = default;
    };

    // Records the shape of a target object as a flat, pre-order tree: a root
    // struct carries the base address expression, its fields carry offsets.
    class Structs
    {
    public:
        std::uint64_t add_struct(std::string name, std::string expression);

        std::uint64_t add_field(std::uint64_t parent, std::string name, std::uint64_t offset, FieldType type);

        bool rename(std::uint64_t id, std::string name);
        bool set_offset(std::uint64_t id, std::uint64_t offset);
        bool set_type(std::uint64_t id, FieldType type);
        bool set_expression(std::uint64_t id, std::string expression);
        bool set_hex(std::uint64_t id, bool hex);

        // Erases the node and its whole subtree, keeping the pre-order invariant.
        bool remove(std::uint64_t id);

        void clear();

        [[nodiscard]] std::span<const StructNode> nodes() const noexcept;
        [[nodiscard]] bool                        empty() const noexcept;
        [[nodiscard]] std::size_t                 size() const noexcept;

        [[nodiscard]] std::size_t depth_of(std::size_t row) const noexcept;

        [[nodiscard]] const StructNode* find(std::uint64_t id) const noexcept;
        [[nodiscard]] std::size_t       index_of(std::uint64_t id) const noexcept;

        [[nodiscard]] std::uint64_t              root_of(std::size_t row) const noexcept;
        [[nodiscard]] std::vector<std::uint64_t> root_ids() const;

        [[nodiscard]] const StructNode* find_struct(std::string_view name) const noexcept;

        [[nodiscard]] std::size_t struct_size(std::uint64_t id, std::size_t pointer_size) const;

        [[nodiscard]] std::vector<std::string> struct_names() const;

    private:
        [[nodiscard]] std::size_t struct_size_at(std::size_t row, std::size_t pointer_size, int budget) const;

        std::vector<StructNode> nodes_;
        std::uint64_t           next_id_ {1};
    };

}
