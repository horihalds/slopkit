#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "reclass/node.hpp"

namespace slopkit::reclass
{
    struct Class
    {
        std::size_t       id {};
        std::string       name;
        std::string       address_expression;
        std::vector<Node> nodes;
    };

    struct Layout
    {
        std::vector<std::size_t> offsets;
        std::size_t              size {};
    };

    class Project
    {
    public:
        [[nodiscard]] const std::vector<Class>& classes() const noexcept;
        [[nodiscard]] std::vector<Class>&       classes() noexcept;

        [[nodiscard]] const Class* class_at(std::size_t index) const noexcept;
        [[nodiscard]] Class*       class_at(std::size_t index) noexcept;

        [[nodiscard]] const Class* find_class(std::string_view name) const noexcept;

        // The new class's id, or zero when the name is empty or already taken.
        std::size_t add_class(std::string name, std::string expression);

        void remove_class(std::size_t index);

        [[nodiscard]] std::size_t next_node_id() noexcept;

        void clear() noexcept;

    private:
        std::vector<Class> classes_;
        std::size_t        next_class_id_ {1};
        std::size_t        next_node_id_ {1};
    };

    // Sequential offsets with no alignment, exactly like the reference's
    // BaseContainerNode.UpdateOffsets; a class that contains itself by value is
    // refused with an error naming the cycle.
    [[nodiscard]] std::expected<Layout, std::string> layout_class(const Project& project, const Class& type);

    // The bytes one node occupies, with a class_instance sized by its target
    // class and an unknown or self-containing target counted as zero.
    [[nodiscard]] std::size_t node_size(const Project& project, const Node& node);

}
