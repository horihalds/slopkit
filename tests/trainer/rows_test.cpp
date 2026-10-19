#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "scan/types.hpp"
#include "trainer/payload.hpp"
#include "trainer/runtime/console.hpp"
#include "trainer/runtime/resolve.hpp"
#include "trainer/runtime/rows.hpp"
#include "trainer/runtime/scripts.hpp"

namespace
{
    using slopkit::scan::ValueType;
    using slopkit::trainer::build_rows;
    using slopkit::trainer::build_script_row;
    using slopkit::trainer::build_value_row;
    using slopkit::trainer::ItemKind;
    using slopkit::trainer::LiveEntry;
    using slopkit::trainer::parse_value_text;
    using slopkit::trainer::Payload;
    using slopkit::trainer::PayloadEntry;
    using slopkit::trainer::PayloadItem;
    using slopkit::trainer::PayloadScript;
    using slopkit::trainer::RowKind;
    using slopkit::trainer::ScriptState;

    PayloadEntry value_entry(std::string description, bool hex = false)
    {
        PayloadEntry entry;
        entry.description = std::move(description);
        entry.type        = ValueType::int32;
        entry.hex         = hex;
        return entry;
    }

    std::vector<std::byte> int_bytes(std::uint32_t value)
    {
        std::vector<std::byte> bytes(4);
        for (std::size_t position = 0; position < bytes.size(); ++position)
        {
            bytes[position] = static_cast<std::byte>((value >> (position * 8)) & 0xff);
        }
        return bytes;
    }
}

TEST_CASE("the rows follow the saved table's order and nesting", "[trainer][rows]")
{
    Payload payload;
    payload.items.push_back(PayloadItem {ItemKind::value, 0, 1, value_entry("Health")});
    payload.items.push_back(PayloadItem {
        ItemKind::script, 0, 1, PayloadScript {"No reload", "return true"}
    });
    payload.items.push_back(PayloadItem {ItemKind::value, 1, 2, value_entry("Health.max")});

    std::vector<LiveEntry> live(2);
    live[0].address  = 0x7f2a12345678;
    live[0].resolved = true;
    live[0].bytes    = int_bytes(42);
    live[1].resolved = false;
    live[1].note     = "unknown module 'practice'";

    std::vector<ScriptState> states(1);
    states[0].failed      = true;
    states[0].interval_ms = 250;
    states[0].message     = "hp went negative";

    const auto rows = build_rows(payload, live, states);
    REQUIRE(rows.size() == 3);

    CHECK(rows[0].kind == RowKind::value);
    CHECK(rows[0].label == "1");
    CHECK(rows[0].number == 1);
    CHECK(rows[0].depth == 0);
    CHECK(rows[0].description == "Health");
    CHECK(rows[0].address == "0x7f2a12345678");
    CHECK(rows[0].value == "42");
    CHECK_FALSE(rows[0].unresolved);

    CHECK(rows[1].kind == RowKind::script);
    CHECK(rows[1].label == "S1");
    CHECK(rows[1].number == 1);
    CHECK(rows[1].depth == 0);
    CHECK(rows[1].description == "No reload");
    CHECK(rows[1].type == "script");
    CHECK(rows[1].address.empty());
    CHECK(rows[1].value.empty());
    CHECK(rows[1].state == "error");
    CHECK(rows[1].interval == "250 ms");
    CHECK(rows[1].message == "hp went negative");

    CHECK(rows[2].kind == RowKind::value);
    CHECK(rows[2].label == "2");
    CHECK(rows[2].number == 2);
    CHECK(rows[2].depth == 1);
    CHECK(rows[2].description == "Health.max");
    CHECK(rows[2].address == "-");
    CHECK(rows[2].unresolved);
    CHECK(rows[2].message == "unknown module 'practice'");
}

TEST_CASE("a script at either end of the table keeps its place", "[trainer][rows]")
{
    Payload payload;
    payload.items.push_back(PayloadItem {
        ItemKind::script, 0, 1, PayloadScript {"First", "return true"}
    });
    payload.items.push_back(PayloadItem {ItemKind::value, 0, 1, value_entry("Health")});
    payload.items.push_back(PayloadItem {
        ItemKind::script, 0, 2, PayloadScript {"Last", "return true"}
    });

    std::vector<LiveEntry>   live(1);
    std::vector<ScriptState> states(2);

    const auto rows = build_rows(payload, live, states);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].kind == RowKind::script);
    CHECK(rows[0].label == "S1");
    CHECK(rows[0].description == "First");
    CHECK(rows[1].kind == RowKind::value);
    CHECK(rows[1].label == "1");
    CHECK(rows[2].kind == RowKind::script);
    CHECK(rows[2].label == "S2");
    CHECK(rows[2].description == "Last");
}

TEST_CASE("a script row reads idle, active or error", "[trainer][rows]")
{
    const PayloadScript script {"No reload", "return true"};
    ScriptState         state;

    CHECK(build_script_row(script, state, 1).state == "idle");
    CHECK(build_script_row(script, state, 1).active == false);

    state.active = true;
    CHECK(build_script_row(script, state, 1).state == "active");
    CHECK(build_script_row(script, state, 1).active);

    state.failed = true;
    CHECK(build_script_row(script, state, 1).state == "error");
}

TEST_CASE("a value row carries the freeze and hex markers", "[trainer][rows]")
{
    const auto entry = value_entry("Health", true);

    LiveEntry live;
    live.address  = 0x7f2a12345678;
    live.resolved = true;
    live.frozen   = true;
    live.bytes    = int_bytes(0xff);

    const auto row = build_value_row(entry, live, 1);
    CHECK(row.frozen);
    CHECK(row.hex);
    CHECK(row.type == "4 Bytes");
    CHECK(row.value == "000000FF");
}

TEST_CASE("the printed value line is the row's fields", "[trainer][rows]")
{
    const auto entry = value_entry("Health");

    LiveEntry live;
    live.address  = 0x7f2a12345678;
    live.resolved = true;
    live.bytes    = int_bytes(42);

    const auto row = build_value_row(entry, live, 1);

    const auto expected = std::format(
        "{:>3}  {:<24}  {:<14}  {:<9}  {:<18}", row.number, row.description, row.address, row.type, row.value);
    CHECK(slopkit::trainer::console::render_value_row(row) == expected);
    CHECK(slopkit::trainer::console::render_value_row(entry, live, 1) == expected);
}

TEST_CASE("the printed script line is the row's fields", "[trainer][rows]")
{
    const PayloadScript script {"No reload", "return true"};
    ScriptState         state;
    state.active      = true;
    state.interval_ms = 250;
    state.message     = "kept hp";

    const auto row = build_script_row(script, state, 2);

    const auto expected = std::format(
        "S{}  {:<24}  {:<14}  {:<9}  {}", row.number, row.description, row.state, row.interval, row.message);
    CHECK(slopkit::trainer::console::render_script_row(row) == expected);
    CHECK(slopkit::trainer::console::render_script_row(script, state, 2) == expected);
}

TEST_CASE("the value editor's parse matches what the entry accepts", "[trainer][rows]")
{
    const auto plain = value_entry("Health");
    CHECK_FALSE(parse_value_text(plain, "42").has_value());
    CHECK_FALSE(parse_value_text(plain, "-7").has_value());

    const auto refused = parse_value_text(plain, "hello");
    REQUIRE(refused.has_value());
    CHECK(refused->find("hello") != std::string::npos);

    const auto hex = value_entry("Flags", true);
    CHECK_FALSE(parse_value_text(hex, "ff").has_value());
    CHECK(parse_value_text(hex, "zz").has_value());
}
