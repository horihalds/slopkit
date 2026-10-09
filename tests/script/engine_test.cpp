#include <catch2/catch.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "support/script_helpers.hpp"

TEST_CASE("script engine reads typed memory", "[script]")
{
    ScriptFixture fixture;
    fixture.fake.put(0x00, {0x01, 0x02, 0x03, 0x04});
    fixture.fake.put_text(0x20, "hello");

    const RunResult result = fixture.engine.run(R"(
print(mem.pointer_size())
print(mem.read(4096, "u32"))
print(mem.read(4098, "u16"))
print(mem.read(4096, "u8"))
print(mem.read(4096, "i32"))
print(mem.read_string(4096 + 0x20))
print(mem.read_bytes(4096, 4))
)");

    REQUIRE(result.ok);
    REQUIRE(result.output.size() == 7);
    CHECK(result.output[0] == "8");
    CHECK(result.output[1] == "67305985");
    CHECK(result.output[2] == "1027");
    CHECK(result.output[3] == "1");
    CHECK(result.output[4] == "67305985");
    CHECK(result.output[5] == "hello");
    CHECK(result.output[6] == std::string("\x01\x02\x03\x04", 4));
}

TEST_CASE("script engine reads floats", "[script]")
{
    ScriptFixture fixture;
    fixture.fake.put(0x00, {0x00, 0x00, 0x80, 0x3F});

    const RunResult result = fixture.engine.run(R"(print(mem.read(4096, "f32")))");

    REQUIRE(result.ok);
    REQUIRE(result.output.size() == 1);
    CHECK(result.output[0] == "1.0");
}

TEST_CASE("script engine writes reach the seam and read back", "[script]")
{
    ScriptFixture fixture;

    const RunResult result = fixture.engine.run(R"(
mem.write(4096, "u32", 0x7FFFFFFF)
print(mem.read(4096, "u32"))
mem.write(4100, "f32", 1.5)
print(mem.read(4100, "f32"))
mem.write_bytes(4110, "\65\66")
print(mem.read_string(4110, 2))
)");

    REQUIRE(result.ok);
    REQUIRE(result.output.size() == 3);
    CHECK(result.output[0] == "2147483647");
    CHECK(result.output[1] == "1.5");
    CHECK(result.output[2] == "AB");

    CHECK(std::to_integer<unsigned>(fixture.fake.bytes[0]) == 0xFF);
    CHECK(std::to_integer<unsigned>(fixture.fake.bytes[1]) == 0xFF);
    CHECK(std::to_integer<unsigned>(fixture.fake.bytes[2]) == 0xFF);
    CHECK(std::to_integer<unsigned>(fixture.fake.bytes[3]) == 0x7F);
    CHECK(std::to_integer<unsigned>(fixture.fake.bytes[14]) == 'A');
}

TEST_CASE("script engine captures print lines and scalar returns", "[script]")
{
    ScriptFixture fixture;

    SECTION("print joins its arguments with a tab")
    {
        const RunResult result = fixture.engine.run(R"(print("a", "b") print(1, nil, true))");

        REQUIRE(result.ok);
        REQUIRE(result.output.size() == 2);
        CHECK(result.output[0] == "a\tb");
        CHECK(result.output[1] == "1\tnil\ttrue");
    }

    SECTION("an integer, a float, a string and a boolean render as its Lua text")
    {
        CHECK(fixture.engine.run("return 42").returned == "42");
        CHECK(fixture.engine.run("return 1.5").returned == "1.5");
        CHECK(fixture.engine.run("return 'hi'").returned == "hi");
        CHECK(fixture.engine.run("return true").returned == "true");
    }

    SECTION("a table, a function and a bare return leave the value empty")
    {
        CHECK(fixture.engine.run("return {1, 2}").returned.empty());
        CHECK(fixture.engine.run("return print").returned.empty());
        CHECK(fixture.engine.run("return").returned.empty());
    }

    SECTION("each run starts with an empty output")
    {
        CHECK(fixture.engine.run("print('x')").output.size() == 1);
        CHECK(fixture.engine.run("return 1").output.empty());
    }
}

TEST_CASE("script engine reports errors instead of throwing", "[script]")
{
    ScriptFixture fixture;

    SECTION("a syntax error")
    {
        const RunResult result = fixture.engine.run("this is not lua");

        CHECK_FALSE(result.ok);
        CHECK(result.error.find("syntax") != std::string::npos);
    }

    SECTION("a runtime error")
    {
        const RunResult result = fixture.engine.run("error('boom')");

        CHECK_FALSE(result.ok);
        CHECK(result.error.find("boom") != std::string::npos);
    }

    SECTION("a failed read carries the seam's message")
    {
        const RunResult result = fixture.engine.run("mem.read(0, 'u32')");

        CHECK_FALSE(result.ok);
        CHECK(result.error.find("address is not mapped") != std::string::npos);
    }

    SECTION("an unknown token is refused")
    {
        const RunResult result = fixture.engine.run("mem.read(4096, 'u24')");

        CHECK_FALSE(result.ok);
        CHECK(result.error.find("unknown value type 'u24'") != std::string::npos);
    }

    SECTION("a failed write carries the seam's message")
    {
        const RunResult result = fixture.engine.run("mem.write(0, 'u8', 1)");

        CHECK_FALSE(result.ok);
        CHECK(result.error.find("address is not mapped") != std::string::npos);
    }

    SECTION("the engine stays usable after a failure")
    {
        CHECK_FALSE(fixture.engine.run("error('boom')").ok);
        CHECK(fixture.engine.run("return 1").ok);
    }

    SECTION("nothing was printed when the syntax is bad")
    {
        CHECK(fixture.engine.run("print('x') print(").output.empty());
    }
}

TEST_CASE("script engine aborts a runaway chunk", "[script]")
{
    EngineConfig config;
    config.instruction_budget = 2000;
    ScriptFixture fixture(0x100, config);

    const RunResult result = fixture.engine.run("while true do end");

    CHECK_FALSE(result.ok);
    CHECK(result.error.find("instruction budget") != std::string::npos);

    // The abort leaves the state usable.
    const RunResult again = fixture.engine.run("return 1");
    CHECK(again.ok);
    CHECK(again.returned == "1");
}

TEST_CASE("script engine aborts on the wall-clock deadline", "[script]")
{
    EngineConfig config;
    config.instruction_budget = 10'000'000'000;
    config.timeout_seconds    = 0.0;
    ScriptFixture fixture(0x100, config);

    const RunResult result = fixture.engine.run("while true do end");

    CHECK_FALSE(result.ok);
    CHECK(result.error.find("time budget") != std::string::npos);
}

TEST_CASE("script engine keeps its state between runs", "[script]")
{
    ScriptFixture fixture;

    REQUIRE(fixture.engine.run("counter = 41\nfunction bump() return counter + 1 end").ok);

    const RunResult result = fixture.engine.run("return bump()");

    REQUIRE(result.ok);
    CHECK(result.returned == "42");
}

TEST_CASE("script engine bounds the captured output", "[script]")
{
    ScriptFixture fixture;

    SECTION("a long line is truncated")
    {
        const RunResult result = fixture.engine.run("print(string.rep('x', 10000))");

        REQUIRE(result.ok);
        REQUIRE(result.output.size() == 1);
        CHECK(result.output[0].size() == slopkit::script::kMaxOutputLine + 3); // the ellipsis
        CHECK(result.output[0].find("x") == 0);
    }

    SECTION("the line count is capped")
    {
        const RunResult result = fixture.engine.run("for i = 1, 5000 do print(i) end");

        REQUIRE(result.ok);
        CHECK(result.output.size() == slopkit::script::kMaxOutputLines);
        CHECK(result.output.front() == "1");
    }
}

TEST_CASE("script engine edge cases at the seam", "[script]")
{
    ScriptFixture fixture;
    fixture.fake.put(0x00, {0x01, 0x02, 0x03, 0x04});

    SECTION("a zero-length byte read is empty")
    {
        const RunResult result = fixture.engine.run("print(mem.read_bytes(4096, 0))");

        REQUIRE(result.ok);
        REQUIRE(result.output.size() == 1);
        CHECK(result.output[0].empty());
    }

    SECTION("an unaligned read is allowed")
    {
        const RunResult result = fixture.engine.run("return mem.read(4097, 'u32')");

        REQUIRE(result.ok);
        CHECK(result.returned == "262914"); // 0x00040302
    }

    SECTION("reading past the end fails")
    {
        const RunResult result = fixture.engine.run("mem.read_bytes(4096, 0x1000)");

        CHECK_FALSE(result.ok);
        CHECK(result.error.find("address is not mapped") != std::string::npos);
    }

    SECTION("a string read that runs off the mapped end fails")
    {
        // The fixture is NUL-free, so the read keeps looking for a terminator
        // until it walks past the last mapped byte.
        std::fill(fixture.fake.bytes.begin(), fixture.fake.bytes.end(), std::byte {'A'});

        const RunResult result = fixture.engine.run("mem.read_string(4096, 512)");

        CHECK_FALSE(result.ok);
        CHECK(result.error.find("address is not mapped") != std::string::npos);
    }
}

TEST_CASE("script engine runs a lifecycle hook", "[script]")
{
    using slopkit::script::kActivateHook;
    using slopkit::script::kDeactivateHook;

    ScriptFixture fixture;

    SECTION("a hook returning nothing succeeds")
    {
        const auto result = fixture.engine.run_lifecycle("function activate() end", kActivateHook);

        REQUIRE(result.ok);
        CHECK(result.error.empty());
        CHECK(result.message.empty());
    }

    SECTION("a hook returning true succeeds")
    {
        const auto result = fixture.engine.run_lifecycle("function activate() return true end", kActivateHook);

        REQUIRE(result.ok);
        CHECK(result.error.empty());
        CHECK(result.message.empty());
    }

    SECTION("a non-boolean first value is treated as success")
    {
        const auto result = fixture.engine.run_lifecycle("function activate() return 1 end", kActivateHook);

        REQUIRE(result.ok);
        CHECK(result.message.empty());
    }

    SECTION("a success may carry a message")
    {
        const auto result =
            fixture.engine.run_lifecycle(R"(function activate() return true, "prepared" end)", kActivateHook);

        REQUIRE(result.ok);
        CHECK(result.message == "prepared");
    }

    SECTION("false refuses and the second value is the message")
    {
        const auto result =
            fixture.engine.run_lifecycle(R"(function activate() return false, "no target object" end)", kActivateHook);

        CHECK_FALSE(result.ok);
        CHECK(result.error.empty());
        CHECK(result.message == "no target object");
    }

    SECTION("false alone refuses without a message")
    {
        const auto result = fixture.engine.run_lifecycle("function activate() return false end", kActivateHook);

        CHECK_FALSE(result.ok);
        CHECK(result.error.empty());
        CHECK(result.message.empty());
    }

    SECTION("a scalar reason is rendered as its Lua text")
    {
        const auto result = fixture.engine.run_lifecycle("function activate() return false, 42 end", kActivateHook);

        CHECK_FALSE(result.ok);
        CHECK(result.message == "42");
    }

    SECTION("the hook's print lines are captured")
    {
        const auto result = fixture.engine.run_lifecycle("function activate() print('hi') end", kActivateHook);

        REQUIRE(result.ok);
        REQUIRE(result.output.size() == 1);
        CHECK(result.output[0] == "hi");
    }

    SECTION("a missing hook is reported")
    {
        const auto result = fixture.engine.run_lifecycle("-- nothing to see", kActivateHook);

        CHECK_FALSE(result.ok);
        CHECK(result.error == "activate() is not defined");
    }

    SECTION("a non-function global is reported")
    {
        const auto result = fixture.engine.run_lifecycle("activate = 1", kActivateHook);

        CHECK_FALSE(result.ok);
        CHECK(result.error == "activate() is not defined");
    }

    SECTION("a chunk error never reaches the hook")
    {
        const auto result = fixture.engine.run_lifecycle(R"(
print('loading')
function activate() print('activated') end
error('boom')
)",
                                                         kActivateHook);

        CHECK_FALSE(result.ok);
        CHECK(result.error.find("boom") != std::string::npos);
        REQUIRE(result.output.size() == 1);
        CHECK(result.output[0] == "loading");
    }

    SECTION("the deactivate route calls deactivate, not activate")
    {
        const auto result = fixture.engine.run_lifecycle(R"(
function activate() return false, "called activate" end
function deactivate() return true end
)",
                                                         kDeactivateHook);

        REQUIRE(result.ok);
        CHECK(result.message.empty());
    }

    SECTION("the state survives and run() still behaves")
    {
        REQUIRE(fixture.engine.run_lifecycle("helper = 7\nfunction activate() end", kActivateHook).ok);

        const RunResult result = fixture.engine.run("return helper + 35");

        REQUIRE(result.ok);
        CHECK(result.returned == "42");
    }
}

TEST_CASE("script engine guards a runaway lifecycle hook", "[script]")
{
    using slopkit::script::kActivateHook;

    SECTION("the instruction budget aborts the hook")
    {
        EngineConfig config;
        config.instruction_budget = 2000;
        ScriptFixture fixture(0x100, config);

        const auto result = fixture.engine.run_lifecycle("function activate() while true do end end", kActivateHook);

        CHECK_FALSE(result.ok);
        CHECK(result.error.find("instruction budget") != std::string::npos);
    }

    SECTION("the wall-clock deadline aborts the hook")
    {
        EngineConfig config;
        config.instruction_budget = 10'000'000'000;
        config.timeout_seconds    = 0.0;
        ScriptFixture fixture(0x100, config);

        const auto result = fixture.engine.run_lifecycle("function activate() while true do end end", kActivateHook);

        CHECK_FALSE(result.ok);
        CHECK(result.error.find("time budget") != std::string::npos);
    }
}

TEST_CASE("script engine registers and sets symbols", "[script]")
{
    ScriptFixture fixture;

    SECTION("rsymbol without a value registers zero")
    {
        REQUIRE(fixture.engine.run(R"(rsymbol("hp"))").ok);
        CHECK(fixture.symbols.lookup("hp") == 0);
    }

    SECTION("rsymbol with a value stores it")
    {
        REQUIRE(fixture.engine.run(R"(rsymbol("hp", 0x1337))").ok);
        CHECK(fixture.symbols.lookup("hp") == 0x1337);
    }

    SECTION("ssymbol sets the value of a fresh name")
    {
        REQUIRE(fixture.engine.run(R"(ssymbol("hp", 1337))").ok);
        CHECK(fixture.symbols.lookup("hp") == 1337);
    }

    SECTION("a later set overwrites the value and keeps one entry")
    {
        REQUIRE(fixture.engine.run(R"(rsymbol("hp", 1) ssymbol("hp", 2))").ok);
        CHECK(fixture.symbols.lookup("hp") == 2);
        CHECK(fixture.symbols.size() == 1);
    }

    SECTION("names are case-insensitive and the first spelling is kept")
    {
        REQUIRE(fixture.engine.run(R"(rsymbol("HP", 1) ssymbol("hp", 2))").ok);
        CHECK(fixture.symbols.size() == 1);
        const std::vector<slopkit::expr::SymbolRef> snapshot = fixture.symbols.snapshot();
        REQUIRE(snapshot.size() == 1);
        CHECK(snapshot.at(0).name == "HP");
    }

    SECTION("the value extremes round-trip")
    {
        REQUIRE(fixture.engine.run(R"(rsymbol("zero", 0) rsymbol("big", 0xFFFFFFFFFFFF))").ok);
        CHECK(fixture.symbols.lookup("zero") == 0);
        CHECK(fixture.symbols.lookup("big") == 0xFFFFFFFFFFFFULL);
    }

    SECTION("a chunk may register many names")
    {
        REQUIRE(fixture.engine.run(R"(for i = 1, 1000 do rsymbol("sym" .. i, i) end)").ok);
        CHECK(fixture.symbols.size() == 1000);
        CHECK(fixture.symbols.lookup("sym1000") == 1000);
    }
}

TEST_CASE("script engine unregisters symbols", "[script]")
{
    ScriptFixture fixture;

    SECTION("usymbol drops the entry and returns nothing")
    {
        const RunResult result = fixture.engine.run(R"(rsymbol("hp", 1) usymbol("hp"))");

        REQUIRE(result.ok);
        CHECK_FALSE(fixture.symbols.lookup("hp").has_value());
        CHECK(result.returned.empty());
    }

    SECTION("a second usymbol is a silent no-op")
    {
        const RunResult result = fixture.engine.run(R"(ssymbol("hp", 1) usymbol("hp") usymbol("hp"))");

        REQUIRE(result.ok);
        CHECK(fixture.symbols.size() == 0);
        CHECK(result.returned.empty());
    }
}

TEST_CASE("script engine reports symbol argument errors", "[script]")
{
    ScriptFixture fixture;

    SECTION("a fractional value is refused")
    {
        const RunResult result = fixture.engine.run(R"(ssymbol("hp", 1.5))");

        CHECK_FALSE(result.ok);
        CHECK(result.error.find("ssymbol: the value must be a 64-bit unsigned integer") != std::string::npos);
        CHECK(fixture.symbols.size() == 0);
    }

    SECTION("a negative value is refused")
    {
        const RunResult result = fixture.engine.run(R"(rsymbol("hp", -1))");

        CHECK_FALSE(result.ok);
        CHECK(result.error.find("rsymbol: the value must be a 64-bit unsigned integer") != std::string::npos);
        CHECK(fixture.symbols.size() == 0);
    }

    SECTION("a non-number value is refused")
    {
        const RunResult result = fixture.engine.run(R"(ssymbol("hp", "1337"))");

        CHECK_FALSE(result.ok);
        CHECK(result.error.find("ssymbol: the value must be a 64-bit unsigned integer") != std::string::npos);
        CHECK(fixture.symbols.size() == 0);
    }

    SECTION("a value past the 64-bit range is refused")
    {
        const RunResult result = fixture.engine.run(R"(ssymbol("hp", 2.0^64))");

        CHECK_FALSE(result.ok);
        CHECK(result.error.find("ssymbol: the value must be a 64-bit unsigned integer") != std::string::npos);
        CHECK(fixture.symbols.size() == 0);
    }

    SECTION("ssymbol without a value is refused")
    {
        const RunResult result = fixture.engine.run(R"(ssymbol("hp"))");

        CHECK_FALSE(result.ok);
        CHECK(result.error.find("ssymbol: the value must be a 64-bit unsigned integer") != std::string::npos);
    }

    SECTION("a non-string name is refused")
    {
        const RunResult result = fixture.engine.run(R"(rsymbol(1, 2))");

        CHECK_FALSE(result.ok);
        CHECK(result.error.find("rsymbol: the name must be a string") != std::string::npos);
    }

    SECTION("a name with a plus is refused with the function name")
    {
        const RunResult result = fixture.engine.run(R"(ssymbol("a+b", 1))");

        CHECK_FALSE(result.ok);
        CHECK(result.error.find("ssymbol: the name must not contain '+'") != std::string::npos);
        CHECK(fixture.symbols.size() == 0);
    }

    SECTION("a name with a leading hash is refused")
    {
        const RunResult result = fixture.engine.run(R"(rsymbol("#hp", 1))");

        CHECK_FALSE(result.ok);
        CHECK(result.error.find("rsymbol: the name must not start with '#'") != std::string::npos);
    }

    SECTION("the engine stays usable after a symbol error")
    {
        CHECK_FALSE(fixture.engine.run(R"(ssymbol("hp", 1.5))").ok);
        CHECK(fixture.engine.run(R"(ssymbol("hp", 2))").ok);
        CHECK(fixture.symbols.lookup("hp") == 2);
    }
}

TEST_CASE("script syntax check compiles without running", "[script]")
{
    SECTION("the dialog's hook skeleton is accepted")
    {
        const std::string skeleton = "function activate()\n"
                                     "    return true\n"
                                     "end\n"
                                     "\n"
                                     "function deactivate()\n"
                                     "    return true\n"
                                     "end\n";
        CHECK(slopkit::script::check_syntax(skeleton).has_value());
    }

    SECTION("a multi-line chunk with tabs and quotes is accepted")
    {
        const std::string chunk = "\tlocal name = 'hi'\n\tprint(\"a\\tb\", name) -- note\n";
        CHECK(slopkit::script::check_syntax(chunk).has_value());
    }

    SECTION("an empty chunk and a comment-only chunk are accepted")
    {
        CHECK(slopkit::script::check_syntax("").has_value());
        CHECK(slopkit::script::check_syntax("-- nothing to do\n").has_value());
    }

    SECTION("a missing end is rejected with the offending line")
    {
        const auto result = slopkit::script::check_syntax("local x = 1\nlocal y = )\n");

        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("script:2:") != std::string::npos);
    }

    SECTION("a chunk that would fail at runtime is still accepted")
    {
        CHECK(slopkit::script::check_syntax(R"(error("boom"))").has_value());
        CHECK(slopkit::script::check_syntax(R"(mem.read(0xDEAD, "u32"))").has_value());
    }
}
