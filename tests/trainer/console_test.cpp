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
    using slopkit::trainer::console::CommandKind;
    using slopkit::trainer::console::parse_command;
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

TEST_CASE("freeze commands accept both spellings", "[trainer][console]")
{
    CHECK(parse_command("f2").kind == CommandKind::freeze);
    CHECK(parse_command("f2").index == 2);
    CHECK(parse_command("f 3").kind == CommandKind::freeze);
    CHECK(parse_command("f 3").index == 3);
    CHECK(parse_command("freeze 12").kind == CommandKind::freeze);
    CHECK(parse_command("freeze 12").index == 12);
}

TEST_CASE("set commands carry the index and the value text", "[trainer][console]")
{
    const auto spaced = parse_command("s 3 100");
    CHECK(spaced.kind == CommandKind::set_value);
    CHECK(spaced.index == 3);
    CHECK(spaced.value == "100");

    const auto joined = parse_command("s3=1f");
    CHECK(joined.kind == CommandKind::set_value);
    CHECK(joined.index == 3);
    CHECK(joined.value == "1f");

    const auto text = parse_command("s 1 hello world");
    CHECK(text.kind == CommandKind::set_value);
    CHECK(text.value == "hello world");

    const auto named = parse_command("set 4 12.5");
    CHECK(named.kind == CommandKind::set_value);
    CHECK(named.index == 4);
    CHECK(named.value == "12.5");
}

TEST_CASE("simple commands are recognized", "[trainer][console]")
{
    CHECK(parse_command("q").kind == CommandKind::quit);
    CHECK(parse_command("quit").kind == CommandKind::quit);
    CHECK(parse_command("r").kind == CommandKind::refresh);
    CHECK(parse_command("refresh").kind == CommandKind::refresh);
    CHECK(parse_command("l").kind == CommandKind::list);
    CHECK(parse_command("list").kind == CommandKind::list);
    CHECK(parse_command("?").kind == CommandKind::help);
    CHECK(parse_command("help").kind == CommandKind::help);
}

TEST_CASE("an empty line is nothing to do", "[trainer][console]")
{
    CHECK(parse_command("").kind == CommandKind::none);
    CHECK(parse_command("   ").kind == CommandKind::none);
    CHECK(parse_command("\t").kind == CommandKind::none);
}

TEST_CASE("bad input is reported as invalid", "[trainer][console]")
{
    CHECK(parse_command("f").kind == CommandKind::invalid);
    CHECK(parse_command("f x").kind == CommandKind::invalid);
    CHECK(parse_command("f x").text == "x");
    CHECK(parse_command("f0").kind == CommandKind::invalid);
    CHECK(parse_command("s 2").kind == CommandKind::invalid);
    CHECK(parse_command("s").kind == CommandKind::invalid);
    CHECK(parse_command("nonsense").kind == CommandKind::invalid);
    CHECK(parse_command("nonsense").text == "nonsense");
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

TEST_CASE("the header counts the scripts too", "[trainer][console]")
{
    auto payload = single_entry_table();
    payload.scripts.push_back(PayloadScript {"No reload", "return true"});

    const auto header = slopkit::trainer::console::render_header(payload, 4242);
    CHECK(header.find("1 script") != std::string::npos);
}

TEST_CASE("activate and deactivate commands accept both spellings", "[trainer][console]")
{
    CHECK(parse_command("a1").kind == CommandKind::activate_script);
    CHECK(parse_command("a1").index == 1);
    CHECK(parse_command("a 2").kind == CommandKind::activate_script);
    CHECK(parse_command("a 2").index == 2);
    CHECK(parse_command("activate 3").kind == CommandKind::activate_script);
    CHECK(parse_command("activate 3").index == 3);
    CHECK(parse_command("d4").kind == CommandKind::deactivate_script);
    CHECK(parse_command("d4").index == 4);
    CHECK(parse_command("d 5").kind == CommandKind::deactivate_script);
    CHECK(parse_command("d 5").index == 5);

    // Missing or unusable indices are reported like the other commands.
    CHECK(parse_command("a").kind == CommandKind::invalid);
    CHECK(parse_command("a x").kind == CommandKind::invalid);
    CHECK(parse_command("a x").text == "x");
    CHECK(parse_command("a0").kind == CommandKind::invalid);
    CHECK(parse_command("d").kind == CommandKind::invalid);

    // `d` never means "deactivate the value entry": the two spaces are apart.
    CHECK(parse_command("f1").kind == CommandKind::freeze);
    CHECK(parse_command("s 1 42").kind == CommandKind::set_value);
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

TEST_CASE("the help and usage text document the script commands", "[trainer][console]")
{
    const auto help = slopkit::trainer::console::render_help();
    CHECK(help.find("a<N>") != std::string::npos);
    CHECK(help.find("d<N>") != std::string::npos);
    CHECK(help.find("independent") != std::string::npos);

    CHECK(slopkit::trainer::console::render_usage().find("--activate N") != std::string::npos);
    CHECK(slopkit::trainer::console::render_usage().find("--verbose") != std::string::npos);
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

    // The interactive list paints these one row at a time, so a row has to render
    // identically whether it is printed or painted.
    const auto rows     = render_rows(payload, std::span {&live, 1}, states);
    const auto expected = slopkit::trainer::console::render_value_row(payload.entries[0], live, 1) + "\n\n"
                        + slopkit::trainer::console::render_script_row(payload.scripts[0], states[0], 1) + "\n";
    CHECK(rows == expected);
}
