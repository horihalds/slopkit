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
        // A run that names what follows - the value cell's `= ` - rather than
        // reading the node's bytes: it keeps the cell's own colour however the
        // value beside it changes.
        bool                       label {};
    };

    // The display text split into runs: the node's own bytes as the first, one
    // reading run for a hex32/hex64 cell's float, integer and raw bits.
    [[nodiscard]] std::vector<ValuePart>
    format_node_value_parts(NodeKind kind, std::span<const std::byte> bytes, bool hex = false);

    // The three readings a hex32/hex64 cell draws beside its bytes.
    enum class NodeReading
    {
        floating,
        integer,
        raw,
    };

    // The readings a hex cell draws, in the order the hints field draws them;
    // empty for every other kind.
    [[nodiscard]] std::vector<NodeReading> node_readings(NodeKind kind);

    // One reading's text, as the hints field draws it between that reading's own
    // parentheses: `0.000`, `-1`, `0xFFFFFFFF` (or `NaN`, `Inf`, `-Inf`).
    [[nodiscard]] std::string format_node_reading(NodeReading reading, NodeKind kind, std::span<const std::byte> bytes);

    // Every reading of a hex32/hex64 cell, each in its own parentheses, one space
    // apart: `(0.000) (42) (0x2A)`; empty for every other kind.
    [[nodiscard]] std::string format_node_readings(NodeKind kind, std::span<const std::byte> bytes);

    // One reading read back in the base that reading is drawn in, so the text
    // format_node_reading drew writes the same bytes back: the integer reading
    // takes bare decimal digits or an `0x` prefix, the raw bits take a bare hex
    // run or `0x`, and the float a decimal float or the drawn `NaN`/`Inf`/`-Inf`.
    // `hex` reads a bare digit run of the integer reading as hex too, and is not
    // the node's own toggle, which governs the value cell alone. A reading of a
    // kind that draws none, a value the node cannot hold and a text that is not a
    // number are refused with the reason.
    [[nodiscard]] std::expected<std::vector<std::byte>, std::string>
    parse_node_reading(NodeReading reading, NodeKind kind, std::string_view text, bool hex = false);

    // The same value as the grid draws it: the codec's text beside the readings
    // the reference shows next to it - an integer's parenthesised hex
    // alternative, a hex node's float and integer readings, a pointer's target.
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
