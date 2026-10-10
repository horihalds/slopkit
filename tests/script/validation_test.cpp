#include <catch2/catch.hpp>

#include <string>

#include "support/script_helpers.hpp"

TEST_CASE("validate reports whether a range is mapped and readable", "[script]")
{
    ScriptFixture fixture(0x200);

    SECTION("a published label and an expression are valid")
    {
        const RunResult result = fixture.engine.run(R"(
local p = alloc("buf", 64)
print(validate(p))
print(validate(label("buf"), 64))
print(validate(expression("0x1000")))
)");

        REQUIRE(result.ok);
        REQUIRE(result.output.size() == 3);
        CHECK(result.output[0] == "true");
        CHECK(result.output[1] == "true");
        CHECK(result.output[2] == "true");
    }

    SECTION("an unmapped address and a range past the buffer are false")
    {
        const RunResult result = fixture.engine.run(R"(
print(validate(0))
print(validate(0x1000, 0x1000))
print(validate(0x1000, 4294967296))
)");

        REQUIRE(result.ok);
        REQUIRE(result.output.size() == 3);
        CHECK(result.output[0] == "false");
        CHECK(result.output[1] == "false");
        CHECK(result.output[2] == "false");
    }

    SECTION("a zero size raises")
    {
        const RunResult result = fixture.engine.run("validate(0x1000, 0)");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("validate: the size must be larger than 0") != std::string::npos);
    }

    SECTION("a non-number size raises")
    {
        const RunResult result = fixture.engine.run("validate(0x1000, 'x')");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("validate: the size must be a number") != std::string::npos);
    }

    SECTION("a refused operation raises with the target's text")
    {
        fixture.fake.validate_error = "the target's plugin cannot validate memory";
        const RunResult result      = fixture.engine.run("validate(0x1000)");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("validate: the target's plugin cannot validate memory") != std::string::npos);
    }

    SECTION("no target is attached")
    {
        const slopkit::script::MemoryApi memory; // no validate seam
        slopkit::script::Engine          engine(memory, fixture.symbols.api());

        const RunResult result = engine.run("validate(0x1000)");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("validate: no target is attached") != std::string::npos);
    }

    SECTION("validate composes with a label and a range")
    {
        const RunResult result = fixture.engine.run(R"(
slabel("base", 0x1000)
print(validate(label("base"), 0x40))
)");

        REQUIRE(result.ok);
        REQUIRE(result.output.size() == 1);
        CHECK(result.output[0] == "true");
    }
}
