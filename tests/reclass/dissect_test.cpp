#include <catch2/catch.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "reclass/dissect.hpp"
#include "reclass/node.hpp"
#include "support/reclass_helpers.hpp"

namespace
{
    using slopkit::reclass::DissectInput;
    using slopkit::reclass::DissectLookup;
    using slopkit::reclass::guess_kind;
    using slopkit::reclass::NodeKind;
    using slopkit::test::bytes_of;

    constexpr std::uint64_t kHeapBase = 0x600000;

    DissectLookup reachable(std::initializer_list<std::uint64_t> addresses)
    {
        const std::vector<std::uint64_t> known {addresses};
        return DissectLookup {.readable = [known](std::uint64_t address, std::size_t)
                              {
                                  return std::ranges::find(known, address) != known.end();
                              }};
    }

    std::optional<NodeKind>
    guess(NodeKind current, std::vector<std::byte> bytes, std::size_t offset, DissectLookup lookup = {})
    {
        return guess_kind(DissectInput {.current = current, .bytes = bytes, .offset = offset, .address = 0x1000},
                          lookup);
    }
}

TEST_CASE("a value that can be dereferenced is guessed as a pointer", "[reclass]")
{
    const std::vector<std::byte> bytes = bytes_of({0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00});

    CHECK(guess(NodeKind::hex64, bytes, 0, reachable({kHeapBase})) == NodeKind::pointer);

    // A value that cannot be read stays with the smaller kinds below it.
    CHECK(guess(NodeKind::hex64, bytes, 0, reachable({})) != NodeKind::pointer);
    CHECK_FALSE(guess(NodeKind::hex64, bytes_of({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}), 0, reachable({0}))
                    .has_value());
}

TEST_CASE("a small float, a small integer and an aligned double are guessed", "[reclass]")
{
    // 1.5f, which is neither a plausible pointer nor zero.
    CHECK(guess(NodeKind::hex32, bytes_of({0x00, 0x00, 0xC0, 0x3F}), 4) == NodeKind::float32);

    // -1000: not a number as a float, in range as a 32-bit integer.
    CHECK(guess(NodeKind::hex32, bytes_of({0x18, 0xFC, 0xFF, 0xFF}), 8) == NodeKind::int32);

    // 1.5 as a double: the low half reads as zero, so only the double fits.
    CHECK(guess(NodeKind::hex64, bytes_of({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x3F}), 8) == NodeKind::float64);

    // The same bytes at a 4-byte offset are not double aligned.
    CHECK(guess(NodeKind::hex64, bytes_of({0x00, 0x00, 0xC0, 0x3F, 0x00, 0x00, 0xF8, 0x3F}), 4) == NodeKind::float32);
}

TEST_CASE("nothing is guessed for a zero, an implausible or a misaligned node", "[reclass]")
{
    CHECK_FALSE(guess(NodeKind::hex32, bytes_of({0x00, 0x00, 0x00, 0x00}), 0).has_value());

    // A million as a double is past the plausible range, and its low half is zero.
    CHECK_FALSE(guess(NodeKind::hex64, bytes_of({0x00, 0x00, 0x00, 0x00, 0x80, 0x84, 0x2E, 0x41}), 0).has_value());

    // The reference skips a node that is not 4-byte aligned.
    CHECK_FALSE(guess(NodeKind::hex32, bytes_of({0x00, 0x00, 0xC0, 0x3F}), 2).has_value());

    // Only a hex node is dissected, and only when its bytes were read.
    CHECK_FALSE(guess(NodeKind::int32, bytes_of({0x00, 0x00, 0xC0, 0x3F}), 0).has_value());
    CHECK_FALSE(guess(NodeKind::hex32, bytes_of({0x00, 0x00}), 0).has_value());
    CHECK_FALSE(guess(NodeKind::hex8, bytes_of({0x00}), 0).has_value());
}
