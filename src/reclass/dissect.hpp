#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

#include "reclass/node.hpp"

namespace slopkit::reclass
{
    struct DissectLookup
    {
        // Whether `size` bytes at `address` can be read from the target.
        std::function<bool(std::uint64_t address, std::size_t size)> readable;
    };

    struct DissectInput
    {
        NodeKind                   current {NodeKind::hex8};
        std::span<const std::byte> bytes;
        std::size_t                offset {};
        std::uint64_t              address {};
    };

    // The reference's NodeDissector.GuessNode ladder restricted to the kinds
    // this version builds: a plausible pointer, then a small float, a small
    // 32-bit integer and an aligned double; anything else stays a hex node.
    [[nodiscard]] std::optional<NodeKind> guess_kind(const DissectInput& input, const DissectLookup& lookup);

}
