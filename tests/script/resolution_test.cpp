#include <catch2/catch.hpp>

#include <cstdint>
#include <string>

#include "support/script_helpers.hpp"

TEST_CASE("label and symbol return the stored value or zero", "[script]")
{
    ScriptFixture fixture;
    REQUIRE(fixture.symbols.set("global", 0x2000).has_value());

    const RunResult result = fixture.engine.run(R"(
slabel("mine", 0x1000)
print(label("mine"))
print(symbol("mine"))
print(label("global"))
print(symbol("global"))
print(label("missing"))
print(symbol("missing"))
)");

    REQUIRE(result.ok);
    REQUIRE(result.output.size() == 6);
    CHECK(result.output[0] == "4096");
    CHECK(result.output[1] == "4096"); // the label answers first
    CHECK(result.output[2] == "0");    // a label never sees the shared table
    CHECK(result.output[3] == "8192");
    CHECK(result.output[4] == "0");
    CHECK(result.output[5] == "0");
}

TEST_CASE("label and symbol reject a malformed name", "[script]")
{
    ScriptFixture fixture;

    SECTION("blank")
    {
        const RunResult result = fixture.engine.run("label('  ')");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("label: the name must not be blank") != std::string::npos);
    }

    SECTION("containing a plus")
    {
        const RunResult result = fixture.engine.run("symbol('a+b')");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("symbol: the name must not contain '+'") != std::string::npos);
    }

    SECTION("starting with a hash")
    {
        const RunResult result = fixture.engine.run("label('#1234')");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("label: the name must not start with '#'") != std::string::npos);
    }

    SECTION("not a string")
    {
        const RunResult result = fixture.engine.run("symbol(5)");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("symbol: the name must be a string") != std::string::npos);
    }
}

TEST_CASE("expression resolves a label before a symbol before a module before a literal", "[script]")
{
    ScriptFixture fixture;
    fixture.fake.modules = {
        {"foo", 0x5000}
    };
    REQUIRE(fixture.symbols.set("foo", 0x2000).has_value());

    const RunResult result = fixture.engine.run(R"(
slabel("foo", 0x1000)
print(expression("foo"))
ulabel("foo")
print(expression("foo"))
usymbol("foo")
print(expression("foo"))
print(expression("1234"))
)");

    REQUIRE(result.ok);
    REQUIRE(result.output.size() == 4);
    CHECK(result.output[0] == "4096");  // the script's own label
    CHECK(result.output[1] == "8192");  // the process-wide symbol
    CHECK(result.output[2] == "20480"); // the module
    CHECK(result.output[3] == "4660");  // a bare literal
}

TEST_CASE("expression follows an offset chain and dereferences", "[script]")
{
    ScriptFixture fixture(0x200);
    // 0x1000 -> 0x1010 and 0x1010 -> 0x1020.
    fixture.fake.put(0x00, {0x10, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    fixture.fake.put(0x10, {0x20, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});

    const RunResult result = fixture.engine.run(R"(
slabel("p", 0x1000)
print(expression("p+0"))
print(expression("p+0x10"))
print(expression("p+0+0"))
print(expression("p+0+0+0"))
)");

    REQUIRE(result.ok);
    REQUIRE(result.output.size() == 4);
    CHECK(result.output[0] == "4096"); // no dereference
    CHECK(result.output[1] == "4112"); // base + 16
    CHECK(result.output[2] == "4112"); // one dereference
    CHECK(result.output[3] == "4128"); // two dereferences
}

TEST_CASE("expression reports a broken or unresolvable text", "[script]")
{
    ScriptFixture fixture;
    fixture.fake.modules = {
        {"libc.so", 0x5000}
    };

    SECTION("syntax")
    {
        const RunResult result = fixture.engine.run("expression('foo+')");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("expression:") != std::string::npos);
    }

    SECTION("unresolvable")
    {
        const RunResult result = fixture.engine.run("expression('nosuchthing')");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("expression:") != std::string::npos);
    }

    SECTION("not a string")
    {
        const RunResult result = fixture.engine.run("expression(5)");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("expression: the expression must be a string") != std::string::npos);
    }

    SECTION("a module name resolves")
    {
        const RunResult result = fixture.engine.run("print(expression('libc.so+0x10'))");
        REQUIRE(result.ok);
        REQUIRE(result.output.size() == 1);
        CHECK(result.output[0] == "20496");
    }
}
