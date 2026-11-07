#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "scan/types.hpp"
#include "trainer/runtime/console.hpp"
#include "trainer/runtime/resolve.hpp"

namespace
{
    using slopkit::scan::ValueType;
    using slopkit::trainer::ItemKind;
    using slopkit::trainer::LiveEntry;
    using slopkit::trainer::Payload;
    using slopkit::trainer::PayloadEntry;
    using slopkit::trainer::PayloadItem;
    using slopkit::trainer::PayloadScript;
    using slopkit::trainer::ScriptState;
    using slopkit::trainer::console::render_rows;

    Payload single_entry_table()
    {
        Payload      payload;
        PayloadEntry entry;
        entry.description = "Health";
        entry.type        = ValueType::int32;
        entry.expression  = "practice+1f2a";
        payload.items.push_back(PayloadItem {ItemKind::value, 0, 1, entry});
        return payload;
    }
}

TEST_CASE("rows carry the value, the freeze marker and the unresolved marker", "[trainer][console]")
{
    const auto payload = single_entry_table();

    std::vector<std::byte> bytes(4, std::byte {0});
    bytes[0] = std::byte {42};

    LiveEntry live;
    live.address  = 0x7f2a12345678;
    live.resolved = true;
    live.bytes    = bytes;

    const auto resolved = render_rows(payload, std::span {&live, 1}, {});
    CHECK(resolved.find("Health") != std::string::npos);
    CHECK(resolved.find("0x7f2a12345678") != std::string::npos);
    CHECK(resolved.find("4 Bytes") != std::string::npos);
    CHECK(resolved.find("42") != std::string::npos);
    CHECK(resolved.find("unresolved") == std::string::npos);
    CHECK(resolved.find("frozen") == std::string::npos);

    live.frozen       = true;
    const auto frozen = render_rows(payload, std::span {&live, 1}, {});
    CHECK(frozen.find("frozen") != std::string::npos);

    live.resolved         = false;
    live.note             = "unknown module or literal 'missing'";
    const auto unresolved = render_rows(payload, std::span {&live, 1}, {});
    CHECK(unresolved.find("unresolved") != std::string::npos);
    CHECK(unresolved.find("missing") != std::string::npos);
}

TEST_CASE("the header names the target and the pointer size", "[trainer][console]")
{
    auto payload         = single_entry_table();
    payload.process_name = "practice";
    payload.pointer_size = 8;

    const auto header = slopkit::trainer::console::render_header(payload, 4242);
    CHECK(header.find("practice") != std::string::npos);
    CHECK(header.find("4242") != std::string::npos);
    CHECK(header.find("1 entry") != std::string::npos);
    CHECK(header.find("8-byte") != std::string::npos);
}

TEST_CASE("the header says the trainer is waiting when no target is attached", "[trainer][console]")
{
    auto payload         = single_entry_table();
    payload.process_name = "practice";

    const auto header = slopkit::trainer::console::render_header(payload, 0);
    CHECK(header.find("waiting for it to appear") != std::string::npos);
    CHECK(header.find("pid 0") == std::string::npos);
}

TEST_CASE("the header counts the scripts too", "[trainer][console]")
{
    auto payload = single_entry_table();
    payload.items.push_back(PayloadItem {
        ItemKind::script, 0, 1, PayloadScript {"No reload", "return true"}
    });

    const auto header = slopkit::trainer::console::render_header(payload, 4242);
    CHECK(header.find("1 script") != std::string::npos);
}

TEST_CASE("the script block shows each script's state, interval and message", "[trainer][console]")
{
    auto payload = single_entry_table();
    payload.items.push_back(PayloadItem {
        ItemKind::script, 0, 1, PayloadScript {"No reload", "return true"}
    });
    payload.items.push_back(PayloadItem {
        ItemKind::script, 0, 2, PayloadScript {"Armour", "return true"}
    });

    std::vector<std::byte> bytes(4, std::byte {0});
    LiveEntry              live;
    live.address  = 0x7f2a12345678;
    live.resolved = true;
    live.bytes    = bytes;

    std::vector<ScriptState> states(2);
    states[1].active      = true;
    states[1].interval_ms = 250;

    const auto rows = render_rows(payload, std::span {&live, 1}, states);
    CHECK(rows.find("Health") != std::string::npos);
    CHECK(rows.find("S1") != std::string::npos);
    CHECK(rows.find("No reload") != std::string::npos);
    CHECK(rows.find("idle") != std::string::npos);
    CHECK(rows.find("S2") != std::string::npos);
    CHECK(rows.find("Armour") != std::string::npos);
    CHECK(rows.find("active") != std::string::npos);
    CHECK(rows.find("250 ms") != std::string::npos);
    CHECK(rows.find("Health") < rows.find("No reload"));
    CHECK(rows.find("No reload") < rows.find("Armour"));

    states[1].active  = false;
    states[1].failed  = true;
    states[1].message = "hp went negative";
    const auto failed = render_rows(payload, std::span {&live, 1}, states);
    CHECK(failed.find("error") != std::string::npos);
    CHECK(failed.find("hp went negative") != std::string::npos);
}

TEST_CASE("the help text documents the window and the usage the flags", "[trainer][console]")
{
    const auto help = slopkit::trainer::console::render_help();
    CHECK(help.find("double-click") != std::string::npos);
    CHECK(help.find("Check column") != std::string::npos);
    CHECK(help.find("order it was saved") != std::string::npos);
    CHECK(help.find("independent") != std::string::npos);
    CHECK(help.find("tooltip") != std::string::npos);

    CHECK(slopkit::trainer::console::render_usage().find("--activate N") != std::string::npos);
    CHECK(slopkit::trainer::console::render_usage().find("--verbose") != std::string::npos);
    CHECK(slopkit::trainer::console::render_usage().find("as soon as it appears") != std::string::npos);
}

TEST_CASE("the list follows the table's order and indents nested rows", "[trainer][console]")
{
    Payload payload;

    PayloadEntry parent;
    parent.description = "Parent";
    parent.type        = ValueType::int32;

    PayloadEntry child;
    child.description = "Parent.child";
    child.type        = ValueType::int32;

    PayloadEntry other;
    other.description = "Other";
    other.type        = ValueType::int32;

    payload.items.push_back(PayloadItem {ItemKind::value, 0, 1, parent});
    payload.items.push_back(PayloadItem {ItemKind::value, 1, 2, child});
    payload.items.push_back(PayloadItem {
        ItemKind::script, 0, 1, PayloadScript {"No reload", "return true"}
    });
    payload.items.push_back(PayloadItem {ItemKind::value, 0, 3, other});

    std::vector<std::byte>   bytes(4, std::byte {0});
    std::vector<LiveEntry>   live(3);
    std::vector<ScriptState> states(1);
    live[0].address  = 0x7f2a12345678;
    live[0].resolved = true;
    live[0].bytes    = bytes;
    live[1].address  = 0x7f2a1234567c;
    live[1].resolved = true;
    live[1].bytes    = bytes;
    live[2].address  = 0x7f2a12345680;
    live[2].resolved = true;
    live[2].bytes    = bytes;

    const auto rows = render_rows(payload, live, states);
    CHECK(rows.find("Parent") < rows.find("No reload"));
    CHECK(rows.find("No reload") < rows.find("Other"));
    CHECK(rows.find("  Parent.child") != std::string::npos);
}

TEST_CASE("the printed list is the row renderers line by line", "[trainer][console]")
{
    auto payload = single_entry_table();
    payload.items.push_back(PayloadItem {
        ItemKind::script, 0, 1, PayloadScript {"No reload", "return true"}
    });

    std::vector<std::byte> bytes(4, std::byte {0});
    bytes[0] = std::byte {42};

    LiveEntry live;
    live.address  = 0x7f2a12345678;
    live.resolved = true;
    live.bytes    = bytes;

    std::vector<ScriptState> states(1);
    states[0].active      = true;
    states[0].interval_ms = 250;

    // The window paints these one row at a time, so a row has to render
    // identically whether it is printed or painted.
    const auto rows     = render_rows(payload, std::span {&live, 1}, states);
    const auto expected = slopkit::trainer::console::render_value_row(*payload.values()[0], live, 1) + "\n"
                        + slopkit::trainer::console::render_script_row(payload.script_records()[0], states[0], 1)
                        + "\n";
    CHECK(rows == expected);
}
