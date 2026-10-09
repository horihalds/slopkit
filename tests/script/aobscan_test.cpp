#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

#include "script/engine.hpp"
#include "script/symbols.hpp"
#include "support/script_helpers.hpp"

TEST_CASE("aobscan stores the first match and returns true", "[script]")
{
    ScriptFixture fixture;
    fixture.fake.put(0x20, {0xde, 0xad, 0x11, 0xef});
    fixture.fake.put(0x30, {0xde, 0xad, 0x22, 0xef});

    const RunResult result = fixture.engine.run(R"(
local ok, addr = aobscan("origin", "de ad ? ef")
print(ok, addr)
)");

    REQUIRE(result.ok);
    REQUIRE(result.output.size() == 1);
    CHECK(result.output[0] == "true\t" + std::to_string(fixture.fake.address(0x20)));
    // The name was new, so it became a label and never reached the shared table.
    CHECK_FALSE(fixture.symbols.lookup("origin").has_value());
}

TEST_CASE("aobscan reports the lowest matching address", "[script]")
{
    ScriptFixture fixture;
    fixture.fake.put(0x50, {0xde, 0xad, 0xbe, 0xef});
    fixture.fake.put(0x10, {0xde, 0xad, 0xbe, 0xef});

    const RunResult result = fixture.engine.run(R"(
local _, addr = aobscan("origin", "de ad be ef")
print(addr)
)");

    REQUIRE(result.ok);
    REQUIRE(result.output.size() == 1);
    CHECK(result.output[0] == std::to_string(fixture.fake.address(0x10)));
}

TEST_CASE("aobscan reports a miss without touching the entry", "[script]")
{
    ScriptFixture fixture;
    fixture.fake.put(0x00, {0x01, 0x02, 0x03});
    REQUIRE(fixture.symbols.set("origin", 0x1111).has_value());

    const RunResult result = fixture.engine.run(R"(
local ok, addr = aobscan("origin", "de ad be ef")
print(ok, addr)
)");

    REQUIRE(result.ok);
    REQUIRE(result.output.size() == 1);
    CHECK(result.output[0] == "false\tnil");
    CHECK(fixture.symbols.lookup("origin") == 0x1111);
}

TEST_CASE("aobscan updates an existing global symbol", "[script]")
{
    ScriptFixture fixture;
    fixture.fake.put(0x40, {0xca, 0xfe});
    REQUIRE(fixture.symbols.set("origin", 0x1111).has_value());

    const RunResult result = fixture.engine.run(R"(aobscan("origin", "ca fe"))");

    REQUIRE(result.ok);
    CHECK(fixture.symbols.lookup("origin") == fixture.fake.address(0x40));
}

TEST_CASE("aobscan honours the module filter", "[script]")
{
    ScriptFixture fixture;
    fixture.fake.put(0x10, {0xde, 0xad, 0xbe, 0xef});
    fixture.fake.put(0x60, {0xde, 0xad, 0xbe, 0xef});
    fixture.fake.regions = {
        MemoryRegion {       .base = kScriptBase, .size = 0x40, .readable = true, .module = "liba.so"},
        MemoryRegion {.base = kScriptBase + 0x40, .size = 0xC0, .readable = true, .module = "libb.so"},
    };

    SECTION("no module walks every readable region")
    {
        const RunResult result = fixture.engine.run(R"(
local _, addr = aobscan("origin", "de ad be ef")
print(addr)
)");

        REQUIRE(result.ok);
        CHECK(result.output[0] == std::to_string(fixture.fake.address(0x10)));
    }

    SECTION("a module restricts the walk to its regions")
    {
        const RunResult result = fixture.engine.run(R"(
local _, addr = aobscan("origin", "de ad be ef", "libb.so")
print(addr)
)");

        REQUIRE(result.ok);
        CHECK(result.output[0] == std::to_string(fixture.fake.address(0x60)));
    }

    SECTION("the module name is matched case-insensitively")
    {
        REQUIRE(fixture.engine.run(R"(aobscan("origin", "de ad be ef", "LIBB.SO"))").ok);
    }

    SECTION("a module whose regions hold no match reports a miss")
    {
        fixture.fake.put(0x10, {0x00, 0x00, 0x00, 0x00});
        const RunResult result = fixture.engine.run(R"(print(aobscan("origin", "de ad be ef", "liba.so")))");

        REQUIRE(result.ok);
        CHECK(result.output[0] == "false");
    }

    SECTION("a module with no matching region is an error")
    {
        const RunResult result = fixture.engine.run(R"(aobscan("origin", "de ad be ef", "nope"))");

        CHECK_FALSE(result.ok);
        CHECK(result.error.find("aobscan: no region belongs to module 'nope'") != std::string::npos);
    }
}

TEST_CASE("a match straddling the read chunk boundary is found", "[script]")
{
    constexpr std::size_t kChunk = 64 * 1024;
    ScriptFixture         fixture(kChunk + 16);
    fixture.fake.put(kChunk - 2, {0xde, 0xad, 0xbe, 0xef});

    const RunResult result = fixture.engine.run(R"(
local _, addr = aobscan("origin", "de ad be ef")
print(addr)
)");

    REQUIRE(result.ok);
    REQUIRE(result.output.size() == 1);
    CHECK(result.output[0] == std::to_string(fixture.fake.address(kChunk - 2)));
}

TEST_CASE("aobscan reports its argument errors", "[script]")
{
    ScriptFixture fixture;

    CHECK(fixture.engine.run(R"(aobscan(5, "de ad"))").error.find("aobscan: the name must be a string")
          != std::string::npos);
    CHECK(fixture.engine.run(R"(aobscan("origin", 5))").error.find("aobscan: the pattern must be a string")
          != std::string::npos);
    CHECK(fixture.engine.run(R"(aobscan("origin", "de ad", 5))").error.find("aobscan: the module must be a string")
          != std::string::npos);
}

TEST_CASE("aobscan refuses a malformed pattern", "[script]")
{
    ScriptFixture fixture;

    CHECK(fixture.engine.run(R"(aobscan("origin", ""))").error.find("aobscan: the pattern is empty")
          != std::string::npos);
    CHECK(fixture.engine.run(R"(aobscan("origin", "zz"))").error.find("aobscan: 'zz' is not a byte")
          != std::string::npos);
    CHECK(fixture.engine.run(R"(aobscan("origin", "?"))").error.find("aobscan: the pattern has no concrete byte")
          != std::string::npos);
}

TEST_CASE("aobscan rejects a bad name before scanning", "[script]")
{
    ScriptFixture fixture;

    const RunResult result = fixture.engine.run(R"(aobscan("#origin", ""))");

    CHECK_FALSE(result.ok);
    CHECK(result.error.find("aobscan: the name must not start with '#'") != std::string::npos);
    CHECK(result.error.find("the pattern is empty") == std::string::npos);
}

TEST_CASE("aobscan reports a target it cannot scan", "[script]")
{
    SECTION("a missing region seam means no target is attached")
    {
        const MemoryApi              memory;
        slopkit::script::SymbolTable symbols;
        Engine                       engine(memory, symbols.api());

        const RunResult result = engine.run(R"(aobscan("origin", "de ad"))");

        CHECK_FALSE(result.ok);
        CHECK(result.error.find("aobscan: no target is attached") != std::string::npos);
    }

    SECTION("a failing region listing carries its text")
    {
        ScriptFixture fixture;
        fixture.fake.regions_error = std::string {"regions unavailable"};

        const RunResult result = fixture.engine.run(R"(aobscan("origin", "de ad"))");

        CHECK_FALSE(result.ok);
        CHECK(result.error.find("aobscan: regions unavailable") != std::string::npos);
    }

    SECTION("a target with no readable memory is refused")
    {
        ScriptFixture fixture;
        fixture.fake.regions = {
            MemoryRegion {.base = kScriptBase, .size = 0x100, .readable = false, .module = "test.so"}
        };

        const RunResult result = fixture.engine.run(R"(aobscan("origin", "de ad"))");

        CHECK_FALSE(result.ok);
        CHECK(result.error.find("aobscan: the target reported no readable memory") != std::string::npos);
    }
}
