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
    using slopkit::expr::SymbolRef;
    using slopkit::expr::Symbols;

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

TEST_CASE("parse reads bracket pointer chains", "[expr]")
{
    auto message_of = [](std::string_view text) -> std::string
    {
        const auto expression = parse(text);
        REQUIRE_FALSE(expression.has_value());
        return expression.error().message;
    };
    auto nested = [](std::size_t depth) -> std::string
    {
        std::string text;
        text.append(depth, '[');
        text += "a";
        text.append(depth, ']');
        return text;
    };

    SECTION("a closing bracket adds one dereference to the group")
    {
        const auto expression = parse_ok("[firefox-bin+10]");
        CHECK(expression.base == "firefox-bin");
        REQUIRE(expression.offsets.size() == 1);
        CHECK(expression.offsets.at(0).value == 0x10);
        CHECK(expression.offsets.at(0).dereferences == 1);
        CHECK(expression.pointer_levels() == 1);
    }

    SECTION("offsets outside a group are added after the dereference")
    {
        const auto expression = parse_ok("[firefox-bin]+10");
        CHECK(expression.base == "firefox-bin");
        REQUIRE(expression.offsets.size() == 2);
        CHECK(expression.offsets.at(0).value == 0);
        CHECK(expression.offsets.at(0).dereferences == 1);
        CHECK(expression.offsets.at(1).value == 0x10);
        CHECK(expression.offsets.at(1).dereferences == 0);
        CHECK(expression.pointer_levels() == 1);
    }

    SECTION("[a+b]+c matches the legacy a+b+c dereference count")
    {
        const auto bracket = parse_ok("[firefox-bin+10]+18");
        const auto legacy  = parse_ok("firefox-bin+10+18");
        CHECK(bracket.pointer_levels() == legacy.pointer_levels());
        CHECK(bracket.pointer_levels() == 1);
        REQUIRE(bracket.offsets.size() == 2);
        CHECK(bracket.offsets.at(0).dereferences == 1);
        CHECK(bracket.offsets.at(1).dereferences == 0);
    }

    SECTION("nesting dereferences twice")
    {
        const auto expression = parse_ok("[[firefox-bin+10]+18]+24");
        CHECK(expression.base == "firefox-bin");
        REQUIRE(expression.offsets.size() == 3);
        CHECK(expression.offsets.at(0).value == 0x10);
        CHECK(expression.offsets.at(0).dereferences == 1);
        CHECK(expression.offsets.at(1).value == 0x18);
        CHECK(expression.offsets.at(1).dereferences == 1);
        CHECK(expression.offsets.at(2).value == 0x24);
        CHECK(expression.offsets.at(2).dereferences == 0);
        CHECK(expression.pointer_levels() == 2);
    }

    SECTION("a bracket on a base with no offsets still dereferences once")
    {
        const auto module = parse_ok("[firefox-bin]");
        REQUIRE(module.offsets.size() == 1);
        CHECK(module.offsets.at(0).value == 0);
        CHECK(module.offsets.at(0).dereferences == 1);
        CHECK(module.pointer_levels() == 1);

        const auto literal = parse_ok("[0x1000]");
        CHECK(literal.base == "0x1000");
        REQUIRE(literal.offsets.size() == 1);
        CHECK(literal.offsets.at(0).dereferences == 1);
    }

    SECTION("whitespace inside a group follows the trimming rule")
    {
        const auto expression = parse_ok("[ firefox-bin + 10 ] + 18");
        CHECK(expression.base == "firefox-bin");
        REQUIRE(expression.offsets.size() == 2);
        CHECK(expression.offsets.at(0).value == 0x10);
        CHECK(expression.offsets.at(0).dereferences == 1);
        CHECK(expression.offsets.at(1).value == 0x18);
    }

    SECTION("a bracket-free expression keeps the legacy dereference pattern")
    {
        const auto expression = parse_ok("firefox-bin+0d+5d+44");
        REQUIRE(expression.offsets.size() == 3);
        CHECK(expression.offsets.at(0).dereferences == 1);
        CHECK(expression.offsets.at(1).dereferences == 1);
        CHECK(expression.offsets.at(2).dereferences == 0);
        CHECK(expression.pointer_levels() == 2);
    }

    SECTION("the nesting cap allows 32 levels and rejects 33")
    {
        CHECK(parse_ok(nested(32)).pointer_levels() == 32);
        CHECK(message_of(nested(33)) == "more than 32 pointer levels");
    }

    SECTION("malformed brackets are rejected with the bracket wording")
    {
        CHECK(message_of("[a") == "unbalanced '['");
        CHECK(message_of("[a+10") == "unbalanced '['");
        CHECK(message_of("a]") == "unbalanced ']'");
        CHECK(message_of("[]") == "an empty '[]' group");
        CHECK(message_of("[+10]") == "missing offset after '+'");
        CHECK(message_of("[a]+") == "missing offset after '+'");
        CHECK(message_of("[a]+zz") == "invalid offset 'zz'");
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
        const auto address = evaluate(parse_ok("FireFox-BIN+10"), modules, Symbols {}, never_read);
        REQUIRE(address.has_value());
        CHECK(*address == 0x400010);
    }

    SECTION("without offsets the base value itself is returned")
    {
        const auto address = evaluate(parse_ok("libc.so.6"), modules, Symbols {}, never_read);
        REQUIRE(address.has_value());
        CHECK(*address == 0x7F000000);
    }

    SECTION("an unknown module falls back to a literal")
    {
        const auto address = evaluate(parse_ok("0x7F001234"), modules, Symbols {}, never_read);
        REQUIRE(address.has_value());
        CHECK(*address == 0x7F001234);
    }

    SECTION("a module whose name looks like a number still wins over the literal")
    {
        const std::vector<ModuleRef> numeric {
            {"deadbeef", 0x1234}
        };
        const auto address = evaluate(parse_ok("deadbeef"), numeric, Symbols {}, never_read);
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

        const auto address = evaluate(parse_ok("firefox-bin+0d+5d+44"), modules, Symbols {}, reader);
        REQUIRE(address.has_value());
        CHECK(*address == 0x2000 + 0x44);

        REQUIRE(reads.size() == 2);
        CHECK(reads.at(0) == 0x400000 + 0x0D);
        CHECK(reads.at(1) == 0x1000 + 0x5D);
    }

    SECTION("an unknown base reports the base failure")
    {
        const auto address = evaluate(parse_ok("foo+10"), modules, Symbols {}, never_read);
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

        const auto address = evaluate(parse_ok("firefox-bin+0d+5d+44"), modules, Symbols {}, reader);
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

        const auto address = evaluate(parse_ok("firefox-bin+0d+5d+44"), modules, Symbols {}, reader);
        REQUIRE_FALSE(address.has_value());
        CHECK(address.error().failed_level == 2);
        CHECK(address.error().message.find("cannot read pointer at level 2") != std::string::npos);
    }
}

TEST_CASE("evaluate resolves symbol bases", "[expr]")
{
    const std::vector<ModuleRef> modules {
        {"firefox-bin",   0x400000},
        {  "libc.so.6", 0x7F000000},
    };
    const std::vector<SymbolRef> symbols {
        {      "hp", 0x1337},
        {"deadbeef", 0xABCD},
        {    "zero",      0},
    };

    PointerReader never_read = [](std::uint64_t) -> std::expected<std::uint64_t, std::string>
    {
        return std::unexpected(std::string {"unexpected read"});
    };

    SECTION("a symbol name resolves to its stored value")
    {
        const auto address = evaluate(parse_ok("hp"), modules, symbols, never_read);
        REQUIRE(address.has_value());
        CHECK(*address == 0x1337);
    }

    SECTION("a symbol is matched case-insensitively")
    {
        const auto address = evaluate(parse_ok("HP"), modules, symbols, never_read);
        REQUIRE(address.has_value());
        CHECK(*address == 0x1337);
    }

    SECTION("an offset is added after the symbol base")
    {
        const auto address = evaluate(parse_ok("hp + 0x10"), modules, symbols, never_read);
        REQUIRE(address.has_value());
        CHECK(*address == 0x1347);
    }

    SECTION("a decimal offset is added after the symbol base")
    {
        const auto address = evaluate(parse_ok("hp + #8"), modules, symbols, never_read);
        REQUIRE(address.has_value());
        CHECK(*address == 0x133F);
    }

    SECTION("a pointer chain dereferences through the reader after the symbol base")
    {
        std::vector<std::uint64_t> reads;
        PointerReader              reader = [&reads](std::uint64_t address) -> std::expected<std::uint64_t, std::string>
        {
            reads.push_back(address);
            if (address == 0x1337 + 0x0D)
            {
                return 0x1000;
            }
            return std::unexpected(std::string {"unexpected read"});
        };

        const auto address = evaluate(parse_ok("hp+0d+5d"), modules, symbols, reader);
        REQUIRE(address.has_value());
        CHECK(*address == 0x105D);

        REQUIRE(reads.size() == 1);
        CHECK(reads.at(0) == 0x1337 + 0x0D);
    }

    SECTION("a symbol named like hex text wins over the bare-hex reading")
    {
        const auto address = evaluate(parse_ok("deadbeef"), modules, symbols, never_read);
        REQUIRE(address.has_value());
        CHECK(*address == 0xABCD);
    }

    SECTION("a zero-valued symbol resolves to zero, not an error")
    {
        const auto address = evaluate(parse_ok("zero"), modules, symbols, never_read);
        REQUIRE(address.has_value());
        CHECK(*address == 0);
    }

    SECTION("a module name still beats a colliding symbol")
    {
        const std::vector<SymbolRef> colliding {
            {"firefox-bin", 0x9999}
        };
        const auto address = evaluate(parse_ok("firefox-bin"), modules, colliding, never_read);
        REQUIRE(address.has_value());
        CHECK(*address == 0x400000);
    }

    SECTION("an unknown name keeps the existing base failure")
    {
        const auto address = evaluate(parse_ok("nope+10"), modules, symbols, never_read);
        REQUIRE_FALSE(address.has_value());
        CHECK(address.error().failed_level == 0);
        CHECK(address.error().message.find("unknown module or literal 'nope'") != std::string::npos);
    }

    SECTION("an empty symbol list changes nothing")
    {
        const auto address = evaluate(parse_ok("firefox-bin+10"), modules, Symbols {}, never_read);
        REQUIRE(address.has_value());
        CHECK(*address == 0x400010);
    }
}

TEST_CASE("evaluate validates every dereference", "[expr]")
{
    using slopkit::expr::AddressValidator;

    const std::vector<ModuleRef> modules {
        {"a", 0x400000},
    };
    const Symbols empty_symbols {};

    const auto pointer_reader = []
    {
        return PointerReader {[](std::uint64_t address) -> std::expected<std::uint64_t, std::string>
                              {
                                  if (address == 0x400000 || address == 0x400010 || address == 0x1018)
                                  {
                                      return 0x1000;
                                  }
                                  return std::unexpected(std::string {"unexpected read"});
                              }};
    };

    SECTION("a bracket chain matches its legacy spelling")
    {
        const auto reader  = pointer_reader();
        const auto bracket = evaluate(parse_ok("[a+10]"), modules, empty_symbols, reader);
        const auto legacy  = evaluate(parse_ok("a+10+0"), modules, empty_symbols, reader);
        REQUIRE(bracket.has_value());
        REQUIRE(legacy.has_value());
        CHECK(*bracket == *legacy);
        CHECK(*bracket == 0x1000);

        // [a]+10 dereferences the base before adding: no legacy spelling.
        const auto deref_base = evaluate(parse_ok("[a]+10"), modules, empty_symbols, reader);
        REQUIRE(deref_base.has_value());
        CHECK(*deref_base == 0x1010);
    }

    SECTION("a nested chain equals its legacy offset chain")
    {
        const auto reader  = pointer_reader();
        const auto bracket = evaluate(parse_ok("[[a+10]+18]+24"), modules, empty_symbols, reader);
        const auto legacy  = evaluate(parse_ok("a+10+18+24"), modules, empty_symbols, reader);
        REQUIRE(bracket.has_value());
        REQUIRE(legacy.has_value());
        CHECK(*bracket == *legacy);
        CHECK(*bracket == 0x1024);
    }

    SECTION("a failing level names the level and the address and skips the read")
    {
        std::size_t         reads  = 0;
        const PointerReader reader = [&reads](std::uint64_t) -> std::expected<std::uint64_t, std::string>
        {
            ++reads;
            return 0x1000;
        };
        const AddressValidator validator = [](std::uint64_t address, std::size_t) -> std::expected<bool, std::string>
        {
            return address != 0x1018;
        };

        const auto resolved = evaluate(parse_ok("a+10+18+24"), modules, empty_symbols, reader, {}, validator);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error().failed_level == 2);
        CHECK(resolved.error().message.find("at level 2") != std::string::npos);
        CHECK(resolved.error().message.find("1018") != std::string::npos);
        // Only level 1 was read; level 2 was validated and skipped.
        CHECK(reads == 1);
    }

    SECTION("an erroring validator is ignored and the reader text stands")
    {
        const PointerReader reader = [](std::uint64_t address) -> std::expected<std::uint64_t, std::string>
        {
            if (address == 0x400010)
            {
                return std::unexpected(std::string {"read failed"});
            }
            return 0x1000;
        };
        const AddressValidator validator = [](std::uint64_t, std::size_t) -> std::expected<bool, std::string>
        {
            return std::unexpected(std::string {"validation unavailable"});
        };

        const auto resolved = evaluate(parse_ok("a+10+18"), modules, empty_symbols, reader, {}, validator);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error().failed_level == 1);
        CHECK(resolved.error().message.find("cannot read pointer at level 1") != std::string::npos);
        CHECK(resolved.error().message.find("read failed") != std::string::npos);
    }

    SECTION("the validator sees the configured pointer size")
    {
        std::vector<std::size_t> sizes;
        const AddressValidator validator = [&sizes](std::uint64_t, std::size_t size) -> std::expected<bool, std::string>
        {
            sizes.push_back(size);
            return true;
        };
        const auto reader = pointer_reader();

        const auto resolved = evaluate(
            parse_ok("[a]+10"), modules, empty_symbols, reader, slopkit::expr::Options {.pointer_size = 4}, validator);
        REQUIRE(resolved.has_value());
        REQUIRE(sizes.size() == 1);
        CHECK(sizes.at(0) == 4);
    }

    SECTION("a bracket expression with no reader fails at its level instead of resolving")
    {
        const PointerReader never_read = [](std::uint64_t) -> std::expected<std::uint64_t, std::string>
        {
            return std::unexpected(std::string {"unexpected read"});
        };

        const auto resolved = evaluate(parse_ok("[a]"), modules, empty_symbols, never_read);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error().failed_level == 1);
        CHECK(resolved.error().message.find("cannot read pointer at level 1") != std::string::npos);
    }
}
