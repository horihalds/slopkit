#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "reclass/nodes/node.hpp"

namespace slopkit::reclass
{
    class Project;

    // The text the grid's Value column shows for a node whose kind carries its
    // own parameters - a text node's length, a bitfield's width, an enum's name;
    // every other kind defers to `format_node_value_display`.
    [[nodiscard]] std::string
    format_node_value_text(const Node& node, const Project& project, std::span<const std::byte> bytes);
    // The text the grid's Value column shows for the bytes read at the node's
    // address.
    [[nodiscard]] std::string format_node_value(NodeKind kind, std::span<const std::byte> bytes, bool hex = false);

    // One run of a value's display text: the node's own bytes, or a reading the
    // cell draws beside them. A reader that paints the cell colours the runs
    // apart; `format_node_value_display` folds them back into one string.
    struct ValuePart
    {
        std::string                text;
        bool                       reading {};
        // The byte this run draws for a hex cell's per-byte dump; absent for a
        // reading and for a kind whose text is not a byte dump.
        std::optional<std::size_t> byte {};
    };

    // The display text split into runs: the node's own bytes as the first, one
    // reading run for a hex32/hex64 cell's float, integer and raw bits.
    [[nodiscard]] std::vector<ValuePart>
    format_node_value_parts(NodeKind kind, std::span<const std::byte> bytes, bool hex = false);

    // The readings a hex32/hex64 cell draws beside its bytes - the float, the
    // integer and the raw bits - without the space the value cell prefixes them
    // with; empty for every other kind.
    [[nodiscard]] std::string format_node_readings(NodeKind kind, std::span<const std::byte> bytes);

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
