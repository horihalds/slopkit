#include <catch2/catch.hpp>

#include <algorithm>
#include <cstddef>
#include <string>

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
