#include <catch2/catch.hpp>

#include <chrono>
#include <cstdint>
#include <string>

#include "debug/breakpoints.hpp"

using slopkit::debug::BreakpointTable;
using slopkit::debug::Kind;

TEST_CASE("breakpoint table allocates software slots in order", "[debug][breakpoints]")
{
    BreakpointTable table;
    CHECK(table.empty());

    const auto first  = table.add("0x1000", 0x1000, Kind::software, 1);
    const auto second = table.add("0x2000", 0x2000, Kind::software, 1);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == 1);
    CHECK(*second == 2);

    REQUIRE(table.find(*first) != nullptr);
    CHECK(table.find(*first)->slot == 0);
    CHECK(table.find(*first)->size == 1);
    CHECK(table.find(*first)->enabled);
    CHECK_FALSE(table.find(*first)->armed);
    CHECK(table.find(*second)->slot == 1);

    CHECK(table.software_at(0x1000) == table.find(*first));
    CHECK(table.software_at(0x3000) == nullptr);
    CHECK(table.entries().size() == 2);
}

TEST_CASE("breakpoint table fills every software slot and reuses a removed one", "[debug][breakpoints]")
{
    BreakpointTable table;
    const auto      started = std::chrono::steady_clock::now();
    for (std::uint32_t i = 0; i < BreakpointTable::software_slots; ++i)
    {
        const auto id = table.add("s", 0x10000 + i * 4, Kind::software, 1);
        REQUIRE(id.has_value());
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < std::chrono::seconds(5));

    const auto overflow = table.add("s", 0x90000, Kind::software, 1);
    REQUIRE_FALSE(overflow.has_value());
    CHECK(overflow.error() == "no software slot is free");

    std::uint64_t removed {};
    for (const auto& entry : table.entries())
    {
        if (entry.slot == 5)
        {
            removed = entry.id;
            break;
        }
    }
    REQUIRE(removed != 0);
    REQUIRE(table.remove(removed));

    const auto reused = table.add("s", 0x90000, Kind::software, 1);
    REQUIRE(reused.has_value());
    CHECK(table.find(*reused)->slot == 5);
}

TEST_CASE("breakpoint table records the hidden flag", "[debug][breakpoints]")
{
    BreakpointTable table;
    const auto      hidden = table.add("m+0x10", 0x1010, Kind::software, 1, true);
    const auto      shown  = table.add("m+0x20", 0x1020, Kind::software, 1);
    REQUIRE(hidden.has_value());
    REQUIRE(shown.has_value());
    CHECK(table.find(*hidden)->hidden);
    CHECK_FALSE(table.find(*shown)->hidden);
}

TEST_CASE("breakpoint table refuses duplicates and exhausted hardware slots", "[debug][breakpoints]")
{
    BreakpointTable table;
    REQUIRE(table.add("a", 0x1000, Kind::software, 1).has_value());

    const auto duplicate = table.add("a", 0x1000, Kind::software, 1);
    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error().find("already") != std::string::npos);

    for (std::uint64_t i = 0; i < BreakpointTable::hardware_slots; ++i)
    {
        REQUIRE(table.add("h", 0x2000 + i * 8, Kind::hardware_write, 4).has_value());
    }
    const auto fifth = table.add("h", 0x3000, Kind::hardware_write, 4);
    REQUIRE_FALSE(fifth.has_value());
    CHECK(fifth.error().find("slot") != std::string::npos);

    for (std::uint32_t slot = 0; slot < BreakpointTable::hardware_slots; ++slot)
    {
        CHECK(table.hardware_in_slot(slot) != nullptr);
    }
    CHECK(table.hardware_in_slot(BreakpointTable::hardware_slots) == nullptr);
}

TEST_CASE("breakpoint sizes follow the kind", "[debug][breakpoints]")
{
    CHECK(BreakpointTable::normalize_size(Kind::software, 8) == 1);
    CHECK(BreakpointTable::normalize_size(Kind::hardware_execute, 8) == 1);
    CHECK(BreakpointTable::normalize_size(Kind::hardware_write, 4) == 4);

    CHECK(BreakpointTable::valid_size(Kind::hardware_write, 1));
    CHECK(BreakpointTable::valid_size(Kind::hardware_write, 2));
    CHECK(BreakpointTable::valid_size(Kind::hardware_write, 4));
    CHECK(BreakpointTable::valid_size(Kind::hardware_write, 8));
    CHECK_FALSE(BreakpointTable::valid_size(Kind::hardware_write, 3));
    CHECK_FALSE(BreakpointTable::valid_size(Kind::hardware_read_write, 16));

    BreakpointTable table;
    const auto      id = table.add("s", 0x1000, Kind::software, 4);
    REQUIRE(id.has_value());
    CHECK(table.find(*id)->size == 1);
}

TEST_CASE("breakpoint table counts hits and removes entries", "[debug][breakpoints]")
{
    BreakpointTable table;
    const auto      id = table.add("e", 0x1000, Kind::hardware_execute, 1);
    REQUIRE(id.has_value());

    REQUIRE(table.count_hit(*id) != nullptr);
    REQUIRE(table.count_hit(*id) != nullptr);
    CHECK(table.find(*id)->hits == 2);
    CHECK(table.count_hit(9999) == nullptr);

    CHECK(table.remove(*id));
    CHECK_FALSE(table.remove(*id));
    CHECK(table.empty());
    CHECK(table.find(*id) == nullptr);
}

TEST_CASE("hardware_kind maps each breakpoint kind to a debug register kind", "[debug][breakpoints]")
{
    using slopkit::debug::hardware_kind;
    using slopkit::debug::HardwareKind;

    CHECK(hardware_kind(Kind::software) == HardwareKind::execute);
    CHECK(hardware_kind(Kind::hardware_execute) == HardwareKind::execute);
    CHECK(hardware_kind(Kind::hardware_write) == HardwareKind::write);
    CHECK(hardware_kind(Kind::hardware_read_write) == HardwareKind::read_write);
}
