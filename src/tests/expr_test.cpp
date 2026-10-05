#include <catch2/catch.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "expr/expression.hpp"
#include "expr/resolver.hpp"

namespace
{
    using slopkit::expr::evaluate;
    using slopkit::expr::Expression;
    using slopkit::expr::ModuleRef;
    using slopkit::expr::parse;
    using slopkit::expr::parse_literal;
    using slopkit::expr::PointerReader;

    Expression parse_ok(std::string_view text)
    {
        const auto expression = parse(text);
        REQUIRE(expression.has_value());
        return *expression;
    }
} // namespace

TEST_CASE("parse_literal reads hexadecimal and decimal literals", "[expr]")
{
    SECTION("prefixed and bare hex")
    {
        CHECK(parse_literal("0x4096").value() == 0x4096);
        CHECK(parse_literal("0X4096").value() == 0x4096);
        CHECK(parse_literal("4096").value() == 0x4096); // a bare token is hex
        CHECK(parse_literal("deadbeef").value() == 0xDEADBEEF);
    }

    SECTION("a leading hash marks decimal")
    {
        CHECK(parse_literal("#50").value() == 50);
        CHECK(parse_literal("#0").value() == 0);
    }

    SECTION("empty and malformed tokens are rejected")
    {
        CHECK_FALSE(parse_literal("").has_value());
        CHECK_FALSE(parse_literal("zz").has_value());
        CHECK_FALSE(parse_literal("0xzz").has_value());
        CHECK_FALSE(parse_literal("0x").has_value());
        CHECK_FALSE(parse_literal("12zz").has_value()); // trailing garbage
    }
}

TEST_CASE("parse splits a base and its offsets", "[expr]")
{
    SECTION("a bare module name carries no offsets")
    {
        const auto expression = parse_ok("firefox-bin");
        CHECK(expression.base == "firefox-bin");
        CHECK(expression.offsets.empty());
        CHECK(expression.pointer_levels() == 0);
    }

    SECTION("bare offsets are hexadecimal")
    {
        CHECK(parse_ok("firefox-bin+50").offsets.at(0).value == 0x50);
        CHECK(parse_ok("firefox-bin+0x4096").offsets.at(0).value == 0x4096);
        CHECK(parse_ok("firefox-bin+deadbeef").offsets.at(0).value == 0xDEADBEEF);
    }

    SECTION("a hash-prefixed offset is decimal")
    {
        const auto expression = parse_ok("firefox-bin+#50");
        REQUIRE(expression.offsets.size() == 1);
        CHECK(expression.offsets.at(0).value == 50);
        CHECK(expression.pointer_levels() == 0);
    }

    SECTION("an absolute base with an offset")
    {
        const auto expression = parse_ok("0x7F001234+10");
        CHECK(expression.base == "0x7F001234");
        REQUIRE(expression.offsets.size() == 1);
        CHECK(expression.offsets.at(0).value == 0x10);
    }

    SECTION("a pointer chain keeps every offset in order")
    {
        const auto expression = parse_ok("firefox-bin+0d+5d+44");
        CHECK(expression.base == "firefox-bin");
        REQUIRE(expression.offsets.size() == 3);
        CHECK(expression.offsets.at(0).value == 0x0D);
        CHECK(expression.offsets.at(1).value == 0x5D);
        CHECK(expression.offsets.at(2).value == 0x44);
        CHECK(expression.pointer_levels() == 2);
    }

    SECTION("whitespace around the tokens is ignored")
    {
        const auto expression = parse_ok("  firefox-bin + 50  ");
        CHECK(expression.base == "firefox-bin");
        REQUIRE(expression.offsets.size() == 1);
        CHECK(expression.offsets.at(0).value == 0x50);
    }

    SECTION("malformed expressions are rejected")
    {
        CHECK_FALSE(parse("").has_value());
        CHECK_FALSE(parse("   ").has_value());
        CHECK_FALSE(parse("+10").has_value());
        CHECK_FALSE(parse("firefox-bin+").has_value());
        CHECK_FALSE(parse("firefox-bin+zz").has_value());
        CHECK_FALSE(parse("firefox-bin++50").has_value());
    }
}

TEST_CASE("evaluate resolves module bases, literals and pointer chains", "[expr]")
{
    const std::vector<ModuleRef> modules {
        {"firefox-bin",   0x400000},
        {  "libc.so.6", 0x7F000000},
    };

    PointerReader never_read = [](std::uint64_t) -> std::expected<std::uint64_t, std::string>
    {
        return std::unexpected(std::string {"unexpected read"});
    };

    SECTION("a module name is looked up case-insensitively")
    {
        const auto address = evaluate(parse_ok("FireFox-BIN+10"), modules, never_read);
        REQUIRE(address.has_value());
        CHECK(*address == 0x400010);
    }

    SECTION("without offsets the base value itself is returned")
    {
        const auto address = evaluate(parse_ok("libc.so.6"), modules, never_read);
        REQUIRE(address.has_value());
        CHECK(*address == 0x7F000000);
    }

    SECTION("an unknown module falls back to a literal")
    {
        const auto address = evaluate(parse_ok("0x7F001234"), modules, never_read);
        REQUIRE(address.has_value());
        CHECK(*address == 0x7F001234);
    }

    SECTION("a module whose name looks like a number still wins over the literal")
    {
        const std::vector<ModuleRef> numeric {
            {"deadbeef", 0x1234}
        };
        const auto address = evaluate(parse_ok("deadbeef"), numeric, never_read);
        REQUIRE(address.has_value());
        CHECK(*address == 0x1234);
    }

    SECTION("a pointer chain dereferences every level with the right address")
    {
        std::vector<std::uint64_t> reads;
        PointerReader              reader = [&reads](std::uint64_t address) -> std::expected<std::uint64_t, std::string>
        {
            reads.push_back(address);
            if (address == 0x400000 + 0x0D)
            {
                return 0x1000;
            }
            if (address == 0x1000 + 0x5D)
            {
                return 0x2000;
            }
            return std::unexpected(std::string {"unexpected read"});
        };

        const auto address = evaluate(parse_ok("firefox-bin+0d+5d+44"), modules, reader);
        REQUIRE(address.has_value());
        CHECK(*address == 0x2000 + 0x44);

        REQUIRE(reads.size() == 2);
        CHECK(reads.at(0) == 0x400000 + 0x0D);
        CHECK(reads.at(1) == 0x1000 + 0x5D);
    }

    SECTION("an unknown base reports the base failure")
    {
        const auto address = evaluate(parse_ok("foo+10"), modules, never_read);
        REQUIRE_FALSE(address.has_value());
        CHECK(address.error().failed_level == 0);
        CHECK(address.error().message.find("unknown module or literal 'foo'") != std::string::npos);
    }

    SECTION("a pointer read failure carries the 1-based level")
    {
        PointerReader reader = [](std::uint64_t address) -> std::expected<std::uint64_t, std::string>
        {
            if (address == 0x400000 + 0x0D)
            {
                return std::unexpected(std::string {"read failed"});
            }
            return 0x2000;
        };

        const auto address = evaluate(parse_ok("firefox-bin+0d+5d+44"), modules, reader);
        REQUIRE_FALSE(address.has_value());
        CHECK(address.error().failed_level == 1);
        CHECK(address.error().message.find("cannot read pointer at level 1") != std::string::npos);
    }

    SECTION("a failure at the last level reports that level")
    {
        PointerReader reader = [](std::uint64_t address) -> std::expected<std::uint64_t, std::string>
        {
            if (address == 0x400000 + 0x0D)
            {
                return 0x1000;
            }
            return std::unexpected(std::string {"read failed"});
        };

        const auto address = evaluate(parse_ok("firefox-bin+0d+5d+44"), modules, reader);
        REQUIRE_FALSE(address.has_value());
        CHECK(address.error().failed_level == 2);
        CHECK(address.error().message.find("cannot read pointer at level 2") != std::string::npos);
    }
}
