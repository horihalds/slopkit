#include "reclass/edit.hpp"

#include <algorithm>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <string>

#include "reclass/nodes/container.hpp"
#include "reclass/nodes/node.hpp"
#include "reclass/project.hpp"

namespace slopkit::reclass
{
    std::vector<NodePtr> pad_nodes(std::size_t bytes)
    {
        std::vector<NodePtr> nodes;
        while (bytes > 0)
        {
            NodeKind kind = NodeKind::hex8;
            if (bytes >= 8)
            {
                kind = NodeKind::hex64;
            }
            else if (bytes >= 4)
            {
                kind = NodeKind::hex32;
            }
            else if (bytes >= 2)
            {
                kind = NodeKind::hex16;
            }

            nodes.push_back(make_node(kind));
            bytes -= kind_size(kind);
        }
        return nodes;
    }

    void append_bytes(Project& project, std::vector<NodePtr>& nodes, std::size_t bytes)
    {
        for (NodePtr& node : pad_nodes(bytes))
        {
            node->id = project.next_node_id();
            nodes.push_back(std::move(node));
        }
    }

    std::string auto_node_name(std::size_t id)
    {
        return std::format("N{:08}", id);
    }

    NodePtr create_node(Project& project, NodeKind kind)
    {
        NodePtr node = make_node(kind);
        node->id     = project.next_node_id();
        if (!is_hex_kind(kind))
        {
            node->name = auto_node_name(node->id);
        }
        return node;
    }

    void change_node_kind(Project& project, std::vector<NodePtr>& nodes, std::size_t index, NodeKind kind)
    {
        if (index >= nodes.size())
        {
            return;
        }

        const std::size_t before = node_size(project, *nodes[index]);

        NodePtr replacement  = make_node(kind);
        replacement->id      = nodes[index]->id;
        replacement->name    = nodes[index]->name;
        replacement->comment = nodes[index]->comment;
        replacement->hex     = nodes[index]->hex;
        replacement->hidden  = nodes[index]->hidden;
        if (is_class_kind(kind))
        {
            replacement->class_name = nodes[index]->class_name;
        }
        // A node that has just been given a type and has no name of its own takes
        // the automatic one; a name the user typed is never touched, and a hex
        // node stays unnamed.
        if (replacement->name.empty() && !is_hex_kind(kind))
        {
            replacement->name = auto_node_name(replacement->id);
        }
        nodes[index] = std::move(replacement);

        const std::size_t after = node_size(project, *nodes[index]);
        if (after >= before)
        {
            return;
        }

        std::vector<NodePtr> padding = pad_nodes(before - after);
        for (NodePtr& node : padding)
        {
            node->id = project.next_node_id();
        }
        nodes.insert(nodes.begin() + static_cast<std::ptrdiff_t>(index) + 1,
                     std::make_move_iterator(padding.begin()),
                     std::make_move_iterator(padding.end()));
    }

    namespace
    {
        std::vector<NodePtr>* owner_of(std::vector<NodePtr>& nodes, std::size_t node_id)
        {
            for (NodePtr& node : nodes)
            {
                if (node->id == node_id)
                {
                    return &nodes;
                }
                if (auto* container = dynamic_cast<ContainerNode*>(node.get()); container != nullptr)
                {
                    if (auto* owner = owner_of(container->children_mutable(), node_id); owner != nullptr)
                    {
                        return owner;
                    }
                }
            }
            return nullptr;
        }
    }

    std::optional<NodeRef> node_ref(Project& project, std::size_t node_id)
    {
        for (Class& type : project.classes())
        {
            std::vector<NodePtr>* owner = owner_of(type.nodes, node_id);
            if (owner == nullptr)
            {
                continue;
            }

            const auto position = std::ranges::find_if(*owner,
                                                       [node_id](const NodePtr& node)
                                                       {
                                                           return node->id == node_id;
                                                       });
            if (position == owner->end())
            {
                continue;
            }

            return NodeRef {.owner = owner, .index = static_cast<std::size_t>(std::distance(owner->begin(), position))};
        }
        return std::nullopt;
    }

    bool remove_node(Project& project, std::size_t node_id)
    {
        const auto ref = node_ref(project, node_id);
        if (!ref)
        {
            return false;
        }
        ref->owner->erase(ref->owner->begin() + static_cast<std::ptrdiff_t>(ref->index));
        return true;
    }

}
