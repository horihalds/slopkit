#pragma once

#include <cstddef>
#include <vector>

#include "reclass/node.hpp"

namespace slopkit::reclass
{
    class Project;

    // Covers `bytes` bytes with padding nodes: 8 -> hex64, >= 4 -> hex32,
    // >= 2 -> hex16, else hex8, the reference's CreateDefaultNodeForSize ladder.
    // The returned nodes carry no id; append_bytes and change_node_kind assign
    // one.
    [[nodiscard]] std::vector<Node> pad_nodes(std::size_t bytes);

    void append_bytes(Project& project, std::vector<Node>& nodes, std::size_t bytes);

    // Re-types `nodes[index]` and pads the freed bytes with hex nodes so the
    // following nodes keep their offsets, the reference's
    // ShouldCompensateSizeChanges behaviour.
    void change_node_kind(Project& project, std::vector<Node>& nodes, std::size_t index, NodeKind kind);

}
