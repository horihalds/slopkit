#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ui/access_watch.hpp"

namespace
{
    using slopkit::debug::RegisterValue;
    using slopkit::disasm::MemoryRef;
    using slopkit::ui::resolve_accesses;
} // namespace

TEST_CASE("a base, index and displacement operand resolves against the registers", "[ui]")
{
    MemoryRef ref;
    ref.base         = "RAX";
    ref.index        = "RCX";
    ref.scale        = 4;
    ref.displacement = 0x10;
    ref.width        = 4;
    ref.writes       = true;

    const std::vector<RegisterValue> values {
        {"RAX", 0x1000},
        {"RCX",   0x10},
    };

    const auto accesses = resolve_accesses(std::span(&ref, 1), 0x4000, 5, values);
    REQUIRE(accesses.size() == 1);
    CHECK(accesses[0].resolved);
    CHECK(accesses[0].address == 0x1000 + 0x10 * 4 + 0x10);
    CHECK(accesses[0].width == 4);
    CHECK(accesses[0].writes);
    CHECK(accesses[0].operand == QStringLiteral("[RAX+RCX*4+0x10]"));
}

TEST_CASE("a rip-relative operand is measured from the end of the instruction", "[ui]")
{
    MemoryRef ref;
    ref.displacement = 0x20;
    ref.width        = 8;
    ref.rip_relative = true;

    const auto accesses = resolve_accesses(std::span(&ref, 1), 0x1000, 7, {});
    REQUIRE(accesses.size() == 1);
    CHECK(accesses[0].resolved);
    CHECK(accesses[0].address == 0x1027);
    CHECK(accesses[0].operand == QStringLiteral("[0x0000000000001027]"));
}

TEST_CASE("an absolute operand resolves to its displacement", "[ui]")
{
    MemoryRef ref;
    ref.displacement = 0x402000;
    ref.width        = 8;

    const auto accesses = resolve_accesses(std::span(&ref, 1), 0x1000, 7, {});
    REQUIRE(accesses.size() == 1);
    CHECK(accesses[0].resolved);
    CHECK(accesses[0].address == 0x402000);
    CHECK(accesses[0].operand == QStringLiteral("[0x0000000000402000]"));
}

TEST_CASE("an operand naming an unknown register stays unresolved", "[ui]")
{
    MemoryRef ref;
    ref.base  = "RAX";
    ref.width = 4;

    const auto accesses = resolve_accesses(std::span(&ref, 1), 0x1000, 3, {});
    REQUIRE(accesses.size() == 1);
    CHECK_FALSE(accesses[0].resolved);
    CHECK(accesses[0].address == 0);
}

TEST_CASE("a 32-bit address register reads the matching 64-bit value", "[ui]")
{
    MemoryRef ref;
    ref.base         = "EAX";
    ref.displacement = 4;
    ref.width        = 4;

    const std::vector<RegisterValue> values {
        {"RAX", 0x2000},
    };

    const auto accesses = resolve_accesses(std::span(&ref, 1), 0x1000, 3, values);
    REQUIRE(accesses.size() == 1);
    CHECK(accesses[0].resolved);
    CHECK(accesses[0].address == 0x2004);
}

TEST_CASE("a negative displacement is subtracted", "[ui]")
{
    MemoryRef ref;
    ref.base         = "RBX";
    ref.displacement = -8;
    ref.width        = 8;

    const std::vector<RegisterValue> values {
        {"RBX", 0x1000},
    };

    const auto accesses = resolve_accesses(std::span(&ref, 1), 0x1000, 3, values);
    REQUIRE(accesses.size() == 1);
    CHECK(accesses[0].address == 0xFF8);
    CHECK(accesses[0].operand == QStringLiteral("[RBX-0x08]"));
}

TEST_CASE("no operands resolve to nothing", "[ui]")
{
    CHECK(resolve_accesses({}, 0x1000, 3, {}).empty());
}

TEST_CASE("a watch width snaps to a hardware data breakpoint size", "[ui]")
{
    CHECK(slopkit::ui::watch_size(1) == 1);
    CHECK(slopkit::ui::watch_size(2) == 2);
    CHECK(slopkit::ui::watch_size(4) == 4);
    CHECK(slopkit::ui::watch_size(8) == 8);
    CHECK(slopkit::ui::watch_size(0) == 4);
    CHECK(slopkit::ui::watch_size(3) == 4);
    CHECK(slopkit::ui::watch_size(16) == 4);
}
