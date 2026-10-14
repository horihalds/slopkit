#include "reclass/nodes/container.hpp"

#include <algorithm>
#include <format>

#include "reclass/project.hpp"

namespace slopkit::reclass
{
    namespace
    {
        std::string count_label(std::size_t count)
        {
            return std::format("[{}]", count);
        }

        void copy_children(const ContainerNode& from, ContainerNode& to)
        {
            for (const NodePtr& child : from.children_mutable())
            {
                to.children_mutable().push_back(child->clone());
            }
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
}
