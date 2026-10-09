#include <catch2/catch.hpp>

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include "scan/pattern.hpp"

namespace
{
    std::vector<std::byte> buffer(std::initializer_list<int> values)
    {
        std::vector<std::byte> bytes;
        bytes.reserve(values.size());
        for (const int value : values)
        {
            bytes.push_back(static_cast<std::byte>(value));
        }
        return bytes;
    }

    slopkit::scan::BytePattern compile(const std::string& text)
    {
        const std::expected<slopkit::scan::BytePattern, std::string> pattern = slopkit::scan::BytePattern::parse(text);
        REQUIRE(pattern.has_value());
        return *pattern;
    }
} // namespace

TEST_CASE("the byte pattern accepts the documented spellings", "[scan]")
{
    using slopkit::scan::BytePattern;

    CHECK(BytePattern::parse("de ad ? ef").value().size() == 4);
    CHECK(BytePattern::parse("de ?? ef").value().size() == 3);
    CHECK(BytePattern::parse("dead?ef").value().size() == 4);
    CHECK(BytePattern::parse("DE AD ? EF").value().size() == 4);
    CHECK(BytePattern::parse("  de\tad\n?  ef ").value().size() == 4);
    CHECK(BytePattern::parse("deadbeef").value().size() == 4);
}

TEST_CASE("the byte pattern rejects malformed text", "[scan]")
{
    using slopkit::scan::BytePattern;

    CHECK(BytePattern::parse("").error() == "the pattern is empty");
    CHECK(BytePattern::parse("   ").error() == "the pattern is empty");
    CHECK(BytePattern::parse("zz").error() == "'zz' is not a byte (use two hex digits or '?')");
    CHECK(BytePattern::parse("de zz").error() == "'zz' is not a byte (use two hex digits or '?')");
    CHECK(BytePattern::parse("de a").error() == "'a' is not a byte (use two hex digits or '?')");
    CHECK(BytePattern::parse("?").error() == "the pattern has no concrete byte");
    CHECK(BytePattern::parse("?? ?").error() == "the pattern has no concrete byte");
    CHECK(BytePattern::parse("???").error()
          == "'???"
             "' is not a byte (use two hex digits or '?')");
}

TEST_CASE("a wildcard matches any byte at its position", "[scan]")
{
    SECTION("a wildcard in the middle")
    {
        const auto pattern = compile("de ? ef");
        CHECK(pattern.find(buffer({0xde, 0x00, 0xef})) == 0);
        CHECK(pattern.find(buffer({0xde, 0xFF, 0xef})) == 0);
        CHECK(pattern.find(buffer({0x00, 0xde, 0x42, 0xef})) == 1);
        CHECK_FALSE(pattern.find(buffer({0xde, 0x42, 0x00})).has_value());
    }

    SECTION("a wildcard at the head")
    {
        const auto pattern = compile("? ef");
        CHECK(pattern.find(buffer({0x00, 0xef})) == 0);
        CHECK(pattern.find(buffer({0x11, 0xef})) == 0);
        CHECK(pattern.find(buffer({0x00, 0x00, 0xef})) == 1);
    }

    SECTION("a wildcard at the tail")
    {
        const auto pattern = compile("de ?");
        CHECK(pattern.find(buffer({0xde, 0x00})) == 0);
        CHECK(pattern.find(buffer({0xde, 0xFF})) == 0);
        CHECK(pattern.find(buffer({0x00, 0xde, 0x11})) == 1);
    }
}

TEST_CASE("the search honours the starting offset", "[scan]")
{
    const auto pattern = compile("de ad");
    const auto bytes   = buffer({0xde, 0xad, 0x00, 0xde, 0xad});

    CHECK(pattern.find(bytes, 0) == 0);
    CHECK(pattern.find(bytes, 1) == 3);
    CHECK(pattern.find(bytes, 2) == 3);
    CHECK(pattern.find(bytes, 4) == std::nullopt);
    CHECK(pattern.find(bytes, 5) == std::nullopt);
}

TEST_CASE("the search handles short and boundary-sized buffers", "[scan]")
{
    SECTION("a miss returns nothing")
    {
        const auto pattern = compile("de ad");
        CHECK_FALSE(pattern.find(buffer({0x00, 0x00, 0x00})).has_value());
    }

    SECTION("a buffer exactly as long as the pattern matches")
    {
        const auto pattern = compile("de ad");
        CHECK(pattern.find(buffer({0xde, 0xad})) == 0);
    }

    SECTION("a pattern longer than the buffer never matches")
    {
        const auto pattern = compile("de ad be ef");
        CHECK_FALSE(pattern.find(buffer({0xde, 0xad})).has_value());
        CHECK_FALSE(pattern.find(buffer({})).has_value());
    }
}
