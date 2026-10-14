#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "reclass/node.hpp"
#include "reclass/nodes/node.hpp"

namespace slopkit::reclass
{
    class Project;

    // The padding a newly created class starts with, so a class is a page of
    // the target the user retypes rather than an empty list.
    inline constexpr std::size_t kDefaultClassBytes = 1024;

    // Covers `bytes` bytes with padding nodes: 8 -> hex64, >= 4 -> hex32,
    // >= 2 -> hex16, else hex8, the reference's CreateDefaultNodeForSize ladder.
    // The returned nodes carry no id; append_bytes and change_node_kind assign
    // one.
    [[nodiscard]] std::vector<NodePtr> pad_nodes(std::size_t bytes);

    void append_bytes(Project& project, std::vector<NodePtr>& nodes, std::size_t bytes);

    // Re-types `nodes[index]` and pads the freed bytes with hex nodes so the
    // following nodes keep their offsets, the reference's
    // ShouldCompensateSizeChanges behaviour.
    void change_node_kind(Project& project, std::vector<NodePtr>& nodes, std::size_t index, NodeKind kind);

    // The vector that holds a node of the project, and the node's index in it.
    struct NodeRef
    {
        std::vector<NodePtr>* owner {};
        std::size_t           index {};
    };

    // Locates the node with this id anywhere in the project, a container's child
    // included.
    [[nodiscard]] std::optional<NodeRef> node_ref(Project& project, std::size_t node_id);

    // Removes the node with this id from wherever it lives.
    bool remove_node(Project& project, std::size_t node_id);

}
