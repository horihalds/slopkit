#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "reclass/nodes/node.hpp"

namespace slopkit::reclass
{
    using PointerNode      = ScalarNode<NodeKind::pointer, 8>;
    using ClassPointerNode = ScalarNode<NodeKind::class_pointer, 8>;

    // A node that owns the nodes nested inside it; the grid expands them the way
    // it expands an inlined class.
    class ContainerNode : public Node
    {
    public:
        [[nodiscard]] std::span<const NodePtr> children() const noexcept override
        {
            return children_;
        }

        [[nodiscard]] bool is_container() const noexcept override
        {
            return true;
        }

        [[nodiscard]] std::vector<NodePtr>& children_mutable() noexcept
        {
            return children_;
        }

        [[nodiscard]] const std::vector<NodePtr>& children_mutable() const noexcept
        {
            return children_;
        }

    protected:
        std::vector<NodePtr> children_;
    };

    // A value-typed class field: the class's bytes are inlined where it sits, so
    // its size is the class it names and it has no value of its own.
    class ClassInstanceNode : public Node
    {
    public:
        [[nodiscard]] NodeKind kind() const noexcept override
        {
            return NodeKind::class_instance;
        }

        [[nodiscard]] std::size_t size(const Project& project) const override;

        [[nodiscard]] std::size_t value_size() const noexcept override
        {
            return 0;
        }

        [[nodiscard]] NodePtr clone() const override
        {
            return std::make_unique<ClassInstanceNode>(*this);
        }
    };

    // An array of elements laid out one after another; the elements are the
    // array's own children, so the grid indents them under the array row.
    class ArrayNode : public ContainerNode
    {
    public:
        [[nodiscard]] NodeKind kind() const noexcept override
        {
            return NodeKind::array;
        }

        [[nodiscard]] std::size_t size(const Project& project) const override;

        [[nodiscard]] std::size_t value_size() const noexcept override
        {
            return 0;
        }

        [[nodiscard]] NodePtr clone() const override;

        [[nodiscard]] std::string_view extra_text() const noexcept override;

        // The array's element count, the number the Type column brackets; it is
        // the element list itself, so growing appends an element and shrinking
        // drops one from the end.
        [[nodiscard]] std::optional<std::size_t> extent() const noexcept override
        {
            return count();
        }

        [[nodiscard]] bool set_extent(Project& project, std::size_t value) override;

        [[nodiscard]] std::size_t count() const noexcept
        {
            return children_.size();
        }

    private:
        mutable std::string extra_cache_;
    };

    // A union: every member shares the union's one address, so the union is as
    // large as its largest member.
    class UnionNode : public ContainerNode
    {
    public:
        [[nodiscard]] NodeKind kind() const noexcept override
        {
            return NodeKind::union_value;
        }

        [[nodiscard]] std::size_t size(const Project& project) const override;

        [[nodiscard]] std::size_t value_size() const noexcept override
        {
            return 0;
        }

        [[nodiscard]] NodePtr clone() const override;
    };

    // A pointer to an array of instances of a class: eight bytes the grid expands
    // into the target class's fields, the way a class pointer is.
    class ClassInstanceArrayNode : public Node
    {
    public:
        std::size_t count {1};

        [[nodiscard]] NodeKind kind() const noexcept override
        {
            return NodeKind::class_instance_array;
        }

        [[nodiscard]] std::size_t size(const Project&) const override
        {
            return 8;
        }

        [[nodiscard]] std::size_t value_size() const noexcept override
        {
            return 8;
        }

        [[nodiscard]] std::size_t repeat() const noexcept override
        {
            return count;
        }

        [[nodiscard]] NodePtr clone() const override;

        [[nodiscard]] std::string_view extra_text() const noexcept override;

        [[nodiscard]] std::optional<std::size_t> extent() const noexcept override
        {
            return count;
        }

        [[nodiscard]] bool set_extent(Project& project, std::size_t value) override;

    private:
        mutable std::string extra_cache_;
    };

    // An array of class values inlined where it sits.
    class ClassWrapperArrayNode : public Node
    {
    public:
        std::size_t count {1};

        [[nodiscard]] NodeKind kind() const noexcept override
        {
            return NodeKind::class_wrapper_array;
        }

        [[nodiscard]] std::size_t size(const Project& project) const override;

        [[nodiscard]] std::size_t value_size() const noexcept override
        {
            return 0;
        }

        [[nodiscard]] std::size_t repeat() const noexcept override
        {
            return count;
        }

        [[nodiscard]] NodePtr clone() const override;

        [[nodiscard]] std::string_view extra_text() const noexcept override;

        [[nodiscard]] std::optional<std::size_t> extent() const noexcept override
        {
            return count;
        }

        [[nodiscard]] bool set_extent(Project& project, std::size_t value) override;

    private:
        mutable std::string extra_cache_;
    };
}
