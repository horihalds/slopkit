#include <catch2/catch.hpp>

#include <cstddef>
#include <string>

#include "support/script_helpers.hpp"

TEST_CASE("alloc maps memory, publishes the name and returns the address", "[script]")
{
    ScriptFixture fixture;

    const RunResult result = fixture.engine.run(R"(
local p = alloc("buf", 64)
print(p)
print(label("buf"))
print(expression("buf"))
write(p, 7)
print(read_u8(p))
dealloc("buf")
)");

    REQUIRE(result.ok);
    REQUIRE(result.output.size() == 4);
    CHECK(result.output[0] == "8192");
    CHECK(result.output[1] == "8192");
    CHECK(result.output[2] == "8192");
    CHECK(result.output[3] == "7");

    REQUIRE(fixture.fake.allocate_requests.size() == 1);
    CHECK(fixture.fake.allocate_requests.front().first == 0);   // no hint
    CHECK(fixture.fake.allocate_requests.front().second == 64); // the size as asked
    CHECK(fixture.fake.allocations.empty());                    // freed again
}

TEST_CASE("alloc passes a near hint through", "[script]")
{
    ScriptFixture fixture;

    const RunResult result = fixture.engine.run(R"(
local p = alloc("buf", 64, expression("0x5000"))
print(p)
)");

    REQUIRE(result.ok);
    REQUIRE(fixture.fake.allocate_requests.size() == 1);
    CHECK(fixture.fake.allocate_requests.front().first == 0x5000);
}

TEST_CASE("alloc publishes with aobscan's label-then-symbol rule", "[script]")
{
    SECTION("an existing label is updated")
    {
        ScriptFixture   fixture;
        const RunResult result = fixture.engine.run(R"(
slabel("buf", 0x1000)
alloc("buf", 64)
print(label("buf"))
)");
        REQUIRE(result.ok);
        REQUIRE(result.output.size() == 1);
        CHECK(result.output[0] == "8192");
    }

    SECTION("an existing global symbol is updated")
    {
        ScriptFixture fixture;
        REQUIRE(fixture.symbols.set("buf", 0x1234).has_value());
        const RunResult result = fixture.engine.run(R"(
alloc("buf", 64)
print(symbol("buf"))
)");
        REQUIRE(result.ok);
        REQUIRE(result.output.size() == 1);
        CHECK(result.output[0] == "8192");
    }
}

TEST_CASE("dealloc refuses an address this session did not allocate", "[script]")
{
    ScriptFixture fixture;
    REQUIRE(fixture.symbols.set("foreign", 0x1000).has_value());

    SECTION("an unknown name")
    {
        const RunResult result = fixture.engine.run("dealloc('nope')");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("dealloc: nope is not this session's allocation") != std::string::npos);
    }

    SECTION("a foreign address")
    {
        const RunResult result = fixture.engine.run("dealloc('foreign')");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("dealloc: foreign is not this session's allocation") != std::string::npos);
    }

    SECTION("freeing twice")
    {
        const RunResult result = fixture.engine.run(R"(
alloc("buf", 64)
dealloc("buf")
dealloc("buf")
)");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("dealloc: buf is not this session's allocation") != std::string::npos);
    }
}

TEST_CASE("alloc reports its own failures", "[script]")
{
    SECTION("zero size")
    {
        ScriptFixture   fixture;
        const RunResult result = fixture.engine.run("alloc('buf', 0)");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("alloc: the size must be larger than 0") != std::string::npos);
    }

    SECTION("negative size")
    {
        ScriptFixture   fixture;
        const RunResult result = fixture.engine.run("alloc('buf', -1)");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("alloc: the size must be larger than 0") != std::string::npos);
    }

    SECTION("a plugin that cannot allocate")
    {
        ScriptFixture fixture;
        fixture.fake.can_allocate = false;
        const RunResult result    = fixture.engine.run("alloc('buf', 64)");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("alloc: the target's plugin cannot allocate memory") != std::string::npos);
    }

    SECTION("a non-number size")
    {
        ScriptFixture   fixture;
        const RunResult result = fixture.engine.run("alloc('buf', 'lots')");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("alloc: the size must be a number") != std::string::npos);
    }

    SECTION("a malformed name")
    {
        ScriptFixture   fixture;
        const RunResult result = fixture.engine.run("alloc('a+b', 64)");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("alloc: the name must not contain '+'") != std::string::npos);
    }
}
