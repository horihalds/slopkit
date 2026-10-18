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
    using slopkit::trainer::LiveEntry;
    using slopkit::trainer::Payload;
    using slopkit::trainer::PayloadEntry;
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
        payload.entries.push_back(entry);
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
    payload.scripts.push_back(PayloadScript {"No reload", "return true"});

    const auto header = slopkit::trainer::console::render_header(payload, 4242);
    CHECK(header.find("1 script") != std::string::npos);
}

TEST_CASE("the script block shows each script's state, interval and message", "[trainer][console]")
{
    auto payload = single_entry_table();
    payload.scripts.push_back(PayloadScript {"No reload", "return true"});
    payload.scripts.push_back(PayloadScript {"Armour", "return true"});

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
    CHECK(help.find("Frozen / Active") != std::string::npos);
    CHECK(help.find("independent") != std::string::npos);

    CHECK(slopkit::trainer::console::render_usage().find("--activate N") != std::string::npos);
    CHECK(slopkit::trainer::console::render_usage().find("--verbose") != std::string::npos);
    CHECK(slopkit::trainer::console::render_usage().find("as soon as it appears") != std::string::npos);
}

TEST_CASE("the printed list is the row renderers line by line", "[trainer][console]")
{
    auto payload = single_entry_table();
    payload.scripts.push_back(PayloadScript {"No reload", "return true"});

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
    const auto expected = slopkit::trainer::console::render_value_row(payload.entries[0], live, 1) + "\n\n"
                        + slopkit::trainer::console::render_script_row(payload.scripts[0], states[0], 1) + "\n";
    CHECK(rows == expected);
}
