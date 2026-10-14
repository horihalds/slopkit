#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "reclass/node.hpp"

namespace slopkit::reclass
{
    // One entry of the node-type menu: a kind and the label the menu shows for
    // it.
    struct NodeMenuEntry
    {
        NodeKind         kind {};
        std::string_view label;
    };

    // A group of the reference's NodeTypesBuilder table; the menu draws a
    // separator between groups.
    struct NodeMenuGroup
    {
        std::string_view           label;
        std::vector<NodeMenuEntry> entries;
    };

    // The reference's node-type groups in the reference's order, shared by the
    // add-node and the change-type menus.
    [[nodiscard]] std::span<const NodeMenuGroup> node_menu_groups();

    // The label the menu shows for a kind, falling back to the taxonomy name for
    // a kind the reference's table does not offer.
    [[nodiscard]] std::string_view node_menu_label(NodeKind kind) noexcept;
}
