#include "reclass/edit.hpp"

#include <iterator>

#include "reclass/project.hpp"

namespace slopkit::reclass
{
    std::vector<Node> pad_nodes(std::size_t bytes)
    {
        std::vector<Node> nodes;
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

            Node node;
            node.kind = kind;
            nodes.push_back(std::move(node));

            bytes -= kind_size(kind);
        }
        return nodes;
    }

    void append_bytes(Project& project, std::vector<Node>& nodes, std::size_t bytes)
    {
        for (Node& node : pad_nodes(bytes))
        {
            node.id = project.next_node_id();
            nodes.push_back(std::move(node));
        }
    }

    void change_node_kind(Project& project, std::vector<Node>& nodes, std::size_t index, NodeKind kind)
    {
        if (index >= nodes.size())
        {
            return;
        }

        const std::size_t before = node_size(project, nodes[index]);
        nodes[index].kind        = kind;
        if (!is_class_kind(kind))
        {
            nodes[index].class_name.clear();
        }
        const std::size_t after = node_size(project, nodes[index]);

        if (after >= before)
        {
            return;
        }

        std::vector<Node> padding = pad_nodes(before - after);
        for (Node& node : padding)
        {
            node.id = project.next_node_id();
        }
        nodes.insert(nodes.begin() + static_cast<std::ptrdiff_t>(index) + 1,
                     std::make_move_iterator(padding.begin()),
                     std::make_move_iterator(padding.end()));
    }

}
