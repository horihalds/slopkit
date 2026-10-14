#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "reclass/node.hpp"

namespace slopkit::reclass
{
    // The text the grid's Value column shows for the bytes read at the node's
    // address.
    [[nodiscard]] std::string format_node_value(NodeKind kind, std::span<const std::byte> bytes, bool hex = false);

    // The bytes a typed value is written back as; `hex` is the node's own base
    // toggle, so bare digits in hex mode read as hexadecimal.
    [[nodiscard]] std::expected<std::vector<std::byte>, std::string>
    parse_node_value(NodeKind kind, std::string_view text, bool hex = false);

}
