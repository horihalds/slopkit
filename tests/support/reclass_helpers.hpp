#pragma once

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "reclass/node.hpp"
#include "reclass/nodes/container.hpp"
#include "reclass/nodes/node.hpp"
#include "reclass/project.hpp"

namespace slopkit::test
{
    [[nodiscard]] inline reclass::NodePtr
    node_of(reclass::Project& project, reclass::NodeKind kind, std::string name = {})
    {
        reclass::NodePtr node = reclass::make_node(kind);
        node->id              = project.next_node_id();
        node->name            = std::move(name);
        return node;
    }

    [[nodiscard]] inline std::vector<std::byte> bytes_of(std::initializer_list<unsigned> values)
    {
        std::vector<std::byte> bytes;
        for (const unsigned value : values)
        {
            bytes.push_back(static_cast<std::byte>(value));
        }
        return bytes;
    }

    // An array of `count` elements of `element`, each with its own id.
    [[nodiscard]] inline reclass::NodePtr
    array_of(reclass::Project& project, std::size_t count, reclass::NodeKind element = reclass::NodeKind::int32)
    {
        auto array = std::make_unique<reclass::ArrayNode>();
        for (std::size_t index = 0; index < count; ++index)
        {
            reclass::NodePtr child = reclass::make_node(element);
            child->id              = project.next_node_id();
            array->children_mutable().push_back(std::move(child));
        }
        array->id = project.next_node_id();
        return array;
    }

    // A union with one member per kind, all at the union's own address.
    [[nodiscard]] inline reclass::NodePtr union_of(reclass::Project&                        project,
                                                   std::initializer_list<reclass::NodeKind> members)
    {
        auto node = std::make_unique<reclass::UnionNode>();
        for (const reclass::NodeKind kind : members)
        {
            reclass::NodePtr child = reclass::make_node(kind);
            child->id              = project.next_node_id();
            node->children_mutable().push_back(std::move(child));
        }
        node->id = project.next_node_id();
        return node;
    }

    // `count` class values inlined where the array sits.
    [[nodiscard]] inline reclass::NodePtr
    wrapper_array_of(reclass::Project& project, std::string class_name, std::size_t count)
    {
        auto node        = std::make_unique<reclass::ClassWrapperArrayNode>();
        node->id         = project.next_node_id();
        node->class_name = std::move(class_name);
        node->count      = count;
        return node;
    }

    // A pointer to `count` instances of a class.
    [[nodiscard]] inline reclass::NodePtr
    instance_array_of(reclass::Project& project, std::string class_name, std::size_t count)
    {
        auto node        = std::make_unique<reclass::ClassInstanceArrayNode>();
        node->id         = project.next_node_id();
        node->class_name = std::move(class_name);
        node->count      = count;
        return node;
    }

}
