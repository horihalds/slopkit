#include <catch2/catch.hpp>

#include <cstddef>
#include <string>
#include <vector>

#include "support/script_helpers.hpp"

TEST_CASE("assemble writes a block into the target and returns its size", "[script]")
{
    ScriptFixture fixture;

    const RunResult result = fixture.engine.run(R"(
local ok, size = assemble(0x1000, "push rbp\nmov rbp, rsp\nsub rsp, 0x20")
print(ok)
print(size)
)");

    REQUIRE(result.ok);
    REQUIRE(result.output.size() == 2);
    CHECK(result.output[0] == "true");
    CHECK(result.output[1] == "8");

    const std::vector<std::byte> expected {std::byte {0x55},
                                           std::byte {0x48},
                                           std::byte {0x89},
                                           std::byte {0xE5},
                                           std::byte {0x48},
                                           std::byte {0x83},
                                           std::byte {0xEC},
                                           std::byte {0x20}};
    REQUIRE(fixture.fake.bytes.size() >= expected.size());
    CHECK(std::vector<std::byte>(fixture.fake.bytes.begin(),
                                 fixture.fake.bytes.begin() + static_cast<std::ptrdiff_t>(expected.size()))
          == expected);
}

TEST_CASE("assemble follows the target's pointer size", "[script]")
{
    SECTION("4-byte pointers encode 32-bit code")
    {
        ScriptFixture fixture;
        fixture.fake.pointer_size = 4;

        const RunResult result = fixture.engine.run(R"(
local ok, size = assemble(0x1000, "push ebp\nmov ebp, esp")
print(ok)
print(size)
)");

        REQUIRE(result.ok);
        REQUIRE(result.output.size() == 2);
        CHECK(result.output[0] == "true");
        CHECK(result.output[1] == "3");
        CHECK(fixture.fake.bytes[0] == std::byte {0x55});
        CHECK(fixture.fake.bytes[1] == std::byte {0x89});
        CHECK(fixture.fake.bytes[2] == std::byte {0xE5});
    }

    SECTION("8-byte pointers refuse `push ebp`")
    {
        ScriptFixture fixture;

        const RunResult result = fixture.engine.run(R"(
local ok, reason = assemble(0x1000, "push ebp")
print(ok)
print(reason)
)");

        REQUIRE(result.ok);
        REQUIRE(result.output.size() == 2);
        CHECK(result.output[0] == "false");
        CHECK(result.output[1].find("line 1") != std::string::npos);
        CHECK(fixture.fake.bytes[0] == std::byte {0});
    }
}

TEST_CASE("assemble reports a rejected line and writes nothing", "[script]")
{
    ScriptFixture fixture;
    fixture.fake.put(0, {0xAA, 0xBB});

    const RunResult result = fixture.engine.run(R"(
local ok, reason = assemble(0x1000, "NOP\nBOGUS RAX")
print(ok)
print(reason)
)");

    REQUIRE(result.ok);
    REQUIRE(result.output.size() == 2);
    CHECK(result.output[0] == "false");
    CHECK(result.output[1] == "line 2: unknown mnemonic 'bogus'");
    CHECK(fixture.fake.bytes[0] == std::byte {0xAA});
    CHECK(fixture.fake.bytes[1] == std::byte {0xBB});
}

TEST_CASE("assemble refuses a text with no instruction", "[script]")
{
    ScriptFixture fixture;

    const RunResult result = fixture.engine.run(R"(
local ok, reason = assemble(0x1000, "; nothing\n  \n")
print(ok)
print(reason)
)");

    REQUIRE(result.ok);
    REQUIRE(result.output.size() == 2);
    CHECK(result.output[0] == "false");
    CHECK(result.output[1] == "the text holds no instruction");
}

TEST_CASE("assemble reports a write the target refuses", "[script]")
{
    ScriptFixture fixture;

    const RunResult result = fixture.engine.run(R"(
local ok, reason = assemble(0x5000, "RET")
print(ok)
print(reason)
)");

    REQUIRE(result.ok);
    REQUIRE(result.output.size() == 2);
    CHECK(result.output[0] == "false");
    CHECK(result.output[1] == "address is not mapped");
}

TEST_CASE("assemble rejects a wrong argument type", "[script]")
{
    SECTION("a non-number address")
    {
        ScriptFixture   fixture;
        const RunResult result = fixture.engine.run("assemble('here', 'NOP')");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("assemble:") != std::string::npos);
    }

    SECTION("a non-string text")
    {
        ScriptFixture   fixture;
        const RunResult result = fixture.engine.run("assemble(0x1000, 42)");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("assemble: the text must be a string") != std::string::npos);
    }
}

TEST_CASE("assemble expands its extra arguments with string.format", "[script]")
{
    ScriptFixture fixture;
    fixture.fake.pointer_size = 4;

    const RunResult result = fixture.engine.run(R"(
local ok, size = assemble(0x1000, "push ebp\nmov ebp, esp\nsub esp, 0x%X", 1337)
print(ok)
print(size)
)");

    REQUIRE(result.ok);
    REQUIRE(result.output.size() == 2);
    CHECK(result.output[0] == "true");
    CHECK(result.output[1] == "9");

    const std::vector<std::byte> expected {std::byte {0x55},
                                           std::byte {0x89},
                                           std::byte {0xE5},
                                           std::byte {0x81},
                                           std::byte {0xEC},
                                           std::byte {0x39},
                                           std::byte {0x05},
                                           std::byte {0x00},
                                           std::byte {0x00}};
    REQUIRE(fixture.fake.bytes.size() >= expected.size());
    CHECK(std::vector<std::byte>(fixture.fake.bytes.begin(),
                                 fixture.fake.bytes.begin() + static_cast<std::ptrdiff_t>(expected.size()))
          == expected);
}

TEST_CASE("assemble leaves a `%` alone when no argument is given", "[script]")
{
    ScriptFixture fixture;

    const RunResult result = fixture.engine.run(R"(
local ok, size = assemble(0x1000, "RET ; 100% done")
print(ok)
print(size)
)");

    REQUIRE(result.ok);
    REQUIRE(result.output.size() == 2);
    CHECK(result.output[0] == "true");
    CHECK(result.output[1] == "1");
    CHECK(fixture.fake.bytes[0] == std::byte {0xC3});
}

TEST_CASE("assemble fails without writing when the format does not expand", "[script]")
{
    ScriptFixture fixture;
    fixture.fake.put(0, {0xAA});

    const RunResult result = fixture.engine.run(R"(
local ok, reason = assemble(0x1000, "NOP\nNOP %X %X", 1)
print(ok)
print(reason)
)");

    REQUIRE(result.ok);
    REQUIRE(result.output.size() == 2);
    CHECK(result.output[0] == "false");
    CHECK(result.output[1].find("format") != std::string::npos);
    CHECK(fixture.fake.bytes[0] == std::byte {0xAA});
}
