#include "reclass/nodes/container.hpp"

#include <algorithm>

#include "reclass/edit.hpp"
#include "reclass/project.hpp"

namespace slopkit::reclass
{
    namespace
    {
        void copy_children(const ContainerNode& from, ContainerNode& to)
        {
            for (const NodePtr& child : from.children_mutable())
            {
                to.children_mutable().push_back(child->clone());
            }
        }

        // The element a grown array appends: the last element's shape, or the
        // array's default element when it holds none, minted with an id and a
        // name of its own exactly like create_node mints a node.
        NodePtr next_array_element(Project& project, const std::vector<NodePtr>& elements)
        {
            if (elements.empty())
            {
                return create_node(project, NodeKind::int8);
            }

            NodePtr element = elements.back()->clone();
            element->id     = project.next_node_id();
            if (!is_hex_kind(element->kind()))
            {
                element->name = auto_node_name(element->id);
            }
            return element;
        }
    }

    std::size_t ClassInstanceNode::size(const Project& project) const
    {
        return node_size(project, *this);
    }

    std::size_t ArrayNode::size(const Project& project) const
    {
        std::size_t total = 0;
        for (const NodePtr& child : children_mutable())
        {
            total += node_size(project, *child);
        }
        return total;
    }

    NodePtr ArrayNode::clone() const
    {
        auto copy                 = std::make_unique<ArrayNode>();
        static_cast<Node&>(*copy) = *this;
        copy_children(*this, *copy);
        return copy;
    }

    std::string_view ArrayNode::extra_text() const noexcept
    {
        extra_cache_ = count_label(children_mutable().size());
        return extra_cache_;
    }

    bool ArrayNode::set_extent(Project& project, std::size_t value)
    {
        if (value == 0)
        {
            return false;
        }

        while (children_mutable().size() > value)
        {
            children_mutable().pop_back();
        }
        while (children_mutable().size() < value)
        {
            children_mutable().push_back(next_array_element(project, children_mutable()));
        }
        return true;
    }

    std::size_t UnionNode::size(const Project& project) const
    {
        std::size_t largest = 0;
        for (const NodePtr& child : children_mutable())
        {
            largest = std::max(largest, node_size(project, *child));
        }
        return largest;
    }

    NodePtr UnionNode::clone() const
    {
        auto copy                 = std::make_unique<UnionNode>();
        static_cast<Node&>(*copy) = *this;
        copy_children(*this, *copy);
        return copy;
    }

    NodePtr ClassInstanceArrayNode::clone() const
    {
        auto copy                 = std::make_unique<ClassInstanceArrayNode>();
        static_cast<Node&>(*copy) = *this;
        copy->count               = count;
        return copy;
    }

    std::string_view ClassInstanceArrayNode::extra_text() const noexcept
    {
        extra_cache_ = count_label(count);
        return extra_cache_;
    }

    bool ClassInstanceArrayNode::set_extent(Project&, std::size_t value)
    {
        if (value == 0)
        {
            return false;
        }
        count = value;
        return true;
    }

    std::size_t ClassWrapperArrayNode::size(const Project& project) const
    {
        return node_size(project, *this);
    }

    NodePtr ClassWrapperArrayNode::clone() const
    {
        auto copy                 = std::make_unique<ClassWrapperArrayNode>();
        static_cast<Node&>(*copy) = *this;
        copy->count               = count;
        return copy;
    }

    std::string_view ClassWrapperArrayNode::extra_text() const noexcept
    {
        extra_cache_ = count_label(count);
        return extra_cache_;
    }

    bool ClassWrapperArrayNode::set_extent(Project&, std::size_t value)
    {
        if (value == 0)
        {
            return false;
        }
        count = value;
        return true;
    }
}
