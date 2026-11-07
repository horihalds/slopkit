#include <catch2/catch.hpp>

#include <cstdint>
#include <string>

#include "support/script_helpers.hpp"

TEST_CASE("script labels stay out of the shared symbol table", "[script]")
{
    ScriptFixture fixture;

    SECTION("registering, setting and removing a label only touch the local table")
    {
        const RunResult result = fixture.engine.run(R"(
rlabel("hp", 7)
slabel("hp", 8)
ulabel("hp")
)");

        REQUIRE(result.ok);
        CHECK(fixture.symbols.size() == 0);
        CHECK_FALSE(fixture.symbols.lookup("hp").has_value());
    }

    SECTION("argument errors name the function")
    {
        CHECK(fixture.engine.run(R"(rlabel(5))").error.find("rlabel: the name must be a string") != std::string::npos);
        CHECK(fixture.engine.run(R"(rlabel("#hp"))").error.find("rlabel: the name must not start with '#'")
              != std::string::npos);
        CHECK(fixture.engine.run(R"(rlabel("a+b"))").error.find("rlabel: the name must not contain '+'")
              != std::string::npos);
        CHECK(
            fixture.engine.run(R"(rlabel("hp", "x"))").error.find("rlabel: the value must be a 64-bit unsigned integer")
            != std::string::npos);
        CHECK(fixture.engine.run(R"(slabel("hp"))").error.find("slabel: the value must be a 64-bit unsigned integer")
              != std::string::npos);
        CHECK(fixture.engine.run(R"(ulabel(5))").error.find("ulabel: the name must be a string") != std::string::npos);
    }
}

TEST_CASE("a label shadows a global symbol of the same name", "[script]")
{
    ScriptFixture fixture;
    fixture.fake.put(0x20, {0xde, 0xad, 0xbe, 0xef});
    REQUIRE(fixture.symbols.set("origin", 0x1111).has_value());

    SECTION("a scan updates the label, leaving the global alone")
    {
        const RunResult result = fixture.engine.run(R"(
rlabel("origin")
aobscan("origin", "de ad be ef")
)");

        REQUIRE(result.ok);
        CHECK(fixture.symbols.lookup("origin") == 0x1111);
    }

    SECTION("the label matches the name case-insensitively")
    {
        REQUIRE(fixture.engine.run(R"(rlabel("ORIGIN") aobscan("origin", "de ad be ef"))").ok);
        CHECK(fixture.symbols.lookup("origin") == 0x1111);
    }

    SECTION("removing the label lets the scan reach the global symbol")
    {
        const RunResult result = fixture.engine.run(R"(
rlabel("origin")
ulabel("origin")
aobscan("origin", "de ad be ef")
)");

        REQUIRE(result.ok);
        CHECK(fixture.symbols.lookup("origin") == fixture.fake.address(0x20));
    }
}

TEST_CASE("labels are scoped to the chunk that registered them", "[script]")
{
    ScriptFixture fixture;
    fixture.fake.put(0x20, {0xde, 0xad, 0xbe, 0xef});
    REQUIRE(fixture.symbols.set("origin", 0x1111).has_value());

    SECTION("a different chunk starts with no labels")
    {
        REQUIRE(fixture.engine.run(R"(rlabel("origin"))").ok);
        REQUIRE(fixture.engine.run(R"(aobscan("origin", "de ad be ef"))").ok);
        CHECK(fixture.symbols.lookup("origin") == fixture.fake.address(0x20));
    }

    SECTION("a script's own hooks share its labels")
    {
        const auto result = fixture.engine.run_lifecycle(R"(
rlabel("origin")
function activate() aobscan("origin", "de ad be ef") end
)",
                                                         std::string {slopkit::script::kActivateHook});

        REQUIRE(result.ok);
        CHECK(fixture.symbols.lookup("origin") == 0x1111);
    }
}
