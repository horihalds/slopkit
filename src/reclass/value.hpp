#pragma once

#include <cstddef>
#include <cstdint>
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

    // The same value as the grid draws it: the codec's text beside the readings
    // the reference shows next to it - an integer's hex form, a hex node's float
    // and integer readings, a pointer's target.
    [[nodiscard]] std::string
    format_node_value_display(NodeKind kind, std::span<const std::byte> bytes, bool hex = false);

    // The little-endian value those bytes read as, for the kinds that have one
    // and a span that covers them; the grid tests a hex node's bytes for an
    // address with it.
    [[nodiscard]] std::uint64_t read_node_value(NodeKind kind, std::span<const std::byte> bytes);

    // The bytes a typed value is written back as; `hex` is the node's own base
    // toggle, so bare digits in hex mode read as hexadecimal.
    [[nodiscard]] std::expected<std::vector<std::byte>, std::string>
    parse_node_value(NodeKind kind, std::string_view text, bool hex = false);

}
