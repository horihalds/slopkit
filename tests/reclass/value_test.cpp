#include <catch2/catch.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "reclass/node.hpp"
#include "reclass/value.hpp"
#include "support/reclass_helpers.hpp"

namespace
{
    using slopkit::reclass::format_node_value;
    using slopkit::reclass::NodeKind;
    using slopkit::reclass::parse_node_value;
    using slopkit::test::bytes_of;

    void require_round_trip(NodeKind kind, std::span<const std::byte> bytes, bool hex)
    {
        const std::string text = format_node_value(kind, bytes, hex);
        REQUIRE_FALSE(text.empty());

        const auto parsed = parse_node_value(kind, text, hex);
        REQUIRE(parsed.has_value());
        REQUIRE(*parsed == std::vector<std::byte>(bytes.begin(), bytes.end()));
    }

    std::vector<std::byte> parsed(NodeKind kind, std::string_view text, bool hex = false)
    {
        const auto bytes = parse_node_value(kind, text, hex);
        REQUIRE(bytes.has_value());
        return *bytes;
    }
}

TEST_CASE("integers render in both bases", "[reclass]")
{
    REQUIRE(format_node_value(NodeKind::int32, bytes_of({0x2A, 0x00, 0x00, 0x00}), false) == "42");
    REQUIRE(format_node_value(NodeKind::int32, bytes_of({0x2A, 0x00, 0x00, 0x00}), true) == "0000002A");
    REQUIRE(format_node_value(NodeKind::int8, bytes_of({0xFF}), false) == "-1");
    REQUIRE(format_node_value(NodeKind::int8, bytes_of({0xFF}), true) == "FF");
    REQUIRE(format_node_value(NodeKind::int16, bytes_of({0x00, 0x80}), false) == "-32768");
    REQUIRE(format_node_value(NodeKind::uint32, bytes_of({0xFF, 0xFF, 0xFF, 0xFF}), false) == "4294967295");
    REQUIRE(format_node_value(NodeKind::uint16, bytes_of({0x10, 0x27}), false) == "10000");
}

TEST_CASE("every integer kind round-trips in both bases", "[reclass]")
{
    for (const bool hex : {false, true})
    {
        require_round_trip(NodeKind::int8, bytes_of({0x80}), hex);
        require_round_trip(NodeKind::int8, bytes_of({0x7F}), hex);
        require_round_trip(NodeKind::int16, bytes_of({0xCD, 0xAB}), hex);
        require_round_trip(NodeKind::int32, bytes_of({0x78, 0x56, 0x34, 0x12}), hex);
        require_round_trip(NodeKind::int64, bytes_of({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}), hex);
        require_round_trip(NodeKind::uint8, bytes_of({0xFE}), hex);
        require_round_trip(NodeKind::uint16, bytes_of({0xFF, 0xFF}), hex);
        require_round_trip(NodeKind::uint32, bytes_of({0x01, 0x00, 0x00, 0x80}), hex);
        require_round_trip(NodeKind::uint64, bytes_of({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}), hex);
    }
}

TEST_CASE("signed integers accept their bounds and refuse what is past them", "[reclass]")
{
    REQUIRE(parsed(NodeKind::int8, "-128") == bytes_of({0x80}));
    REQUIRE(parsed(NodeKind::int8, "127") == bytes_of({0x7F}));
    REQUIRE_FALSE(parse_node_value(NodeKind::int8, "128").has_value());
    REQUIRE_FALSE(parse_node_value(NodeKind::int8, "-129").has_value());

    REQUIRE(parsed(NodeKind::int64, "-9223372036854775808")
            == bytes_of({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80}));
    REQUIRE(parsed(NodeKind::int32, "0xFFFFFFFF") == bytes_of({0xFF, 0xFF, 0xFF, 0xFF}));
}

TEST_CASE("unsigned integers are readable in either base", "[reclass]")
{
    REQUIRE(parsed(NodeKind::uint8, "255") == bytes_of({0xFF}));
    REQUIRE_FALSE(parse_node_value(NodeKind::uint8, "256").has_value());
    REQUIRE_FALSE(parse_node_value(NodeKind::uint8, "-1").has_value());
    REQUIRE(parsed(NodeKind::uint8, "0x1FF") == bytes_of({0xFF}));
    REQUIRE(parsed(NodeKind::uint16, "0x1234") == bytes_of({0x34, 0x12}));
}

TEST_CASE("a booleans renders and parses as true or false", "[reclass]")
{
    REQUIRE(format_node_value(NodeKind::boolean, bytes_of({0x01}), false) == "true");
    REQUIRE(format_node_value(NodeKind::boolean, bytes_of({0x00}), false) == "false");
    REQUIRE(parsed(NodeKind::boolean, "true") == bytes_of({0x01}));
    REQUIRE(parsed(NodeKind::boolean, "TRUE") == bytes_of({0x01}));
    REQUIRE(parsed(NodeKind::boolean, "1") == bytes_of({0x01}));
    REQUIRE(parsed(NodeKind::boolean, "false") == bytes_of({0x00}));
    REQUIRE_FALSE(parse_node_value(NodeKind::boolean, "maybe").has_value());
}

TEST_CASE("a character renders as itself or as an escape", "[reclass]")
{
    REQUIRE(format_node_value(NodeKind::character, bytes_of({0x41}), false) == "A");
    REQUIRE(format_node_value(NodeKind::character, bytes_of({0x0A}), false) == "\\x0A");
    REQUIRE(parsed(NodeKind::character, "A") == bytes_of({0x41}));
    REQUIRE(parsed(NodeKind::character, "\\x0A") == bytes_of({0x0A}));
    REQUIRE_FALSE(parse_node_value(NodeKind::character, "AB").has_value());
    REQUIRE_FALSE(parse_node_value(NodeKind::character, "\\x1FF").has_value());
}

TEST_CASE("hex nodes render and parse as bare upper-case hex", "[reclass]")
{
    REQUIRE(format_node_value(NodeKind::hex32, bytes_of({0x11, 0x22, 0x33, 0x44}), false) == "11223344");
    REQUIRE(format_node_value(NodeKind::hex8, bytes_of({0x0C}), false) == "0C");

    REQUIRE(parsed(NodeKind::hex32, "11223344") == bytes_of({0x11, 0x22, 0x33, 0x44}));
    REQUIRE(parsed(NodeKind::hex32, "0x11223344") == bytes_of({0x11, 0x22, 0x33, 0x44}));
    REQUIRE(parsed(NodeKind::hex16, "11 22") == bytes_of({0x11, 0x22}));
    REQUIRE(parsed(NodeKind::hex64, "0102030405060708") == bytes_of({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}));

    const auto short_digits = parse_node_value(NodeKind::hex32, "1122");
    REQUIRE_FALSE(short_digits.has_value());
    REQUIRE(short_digits.error().find("8") != std::string::npos);
    REQUIRE_FALSE(parse_node_value(NodeKind::hex8, "GG").has_value());
}

TEST_CASE("floats render decimal by default and raw hex on request", "[reclass]")
{
    REQUIRE(format_node_value(NodeKind::float32, bytes_of({0x00, 0x00, 0xC0, 0x3F}), false) == "1.5");
    REQUIRE(format_node_value(NodeKind::float32, bytes_of({0x00, 0x00, 0xC0, 0x3F}), true) == "3FC00000");

    REQUIRE(parsed(NodeKind::float32, "1.5") == bytes_of({0x00, 0x00, 0xC0, 0x3F}));
    REQUIRE(parsed(NodeKind::float32, "-2.25") == bytes_of({0x00, 0x00, 0x10, 0xC0}));
    REQUIRE(parsed(NodeKind::float32, "3FC00000", true) == bytes_of({0x00, 0x00, 0xC0, 0x3F}));
    REQUIRE_FALSE(parse_node_value(NodeKind::float32, "1.5.5").has_value());

    require_round_trip(NodeKind::float32, bytes_of({0x00, 0x00, 0xC0, 0x3F}), false);
    require_round_trip(NodeKind::float64, bytes_of({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x3F}), false);
    require_round_trip(NodeKind::float64, bytes_of({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}), true);
}

TEST_CASE("a pointer renders as a hex address", "[reclass]")
{
    REQUIRE(format_node_value(NodeKind::pointer, bytes_of({0x34, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}), false)
            == "0x1234");
    REQUIRE(format_node_value(NodeKind::pointer, bytes_of({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}), false)
            == "0x0");

    REQUIRE(parsed(NodeKind::pointer, "0x1234") == bytes_of({0x34, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    REQUIRE(parsed(NodeKind::pointer, "#4660") == bytes_of({0x34, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    REQUIRE_FALSE(parse_node_value(NodeKind::pointer, "-1").has_value());
}

TEST_CASE("a value too short for its kind has no rendering", "[reclass]")
{
    REQUIRE(format_node_value(NodeKind::int32, bytes_of({0x01, 0x02}), false).empty());
    REQUIRE(format_node_value(NodeKind::float64, bytes_of({}), false).empty());
    REQUIRE(format_node_value(NodeKind::class_instance, bytes_of({0x01, 0x02, 0x03, 0x04}), false).empty());
    REQUIRE_FALSE(parse_node_value(NodeKind::class_instance, "1").has_value());
}

TEST_CASE("the grid's reading of a value adds the reference's alternatives", "[reclass]")
{
    using slopkit::reclass::format_node_value_display;
    using slopkit::reclass::read_node_value;

    // An integer with the hex form beside it, and the other way round once the
    // node is shown as hex.
    REQUIRE(format_node_value_display(NodeKind::int32, bytes_of({0x2A, 0x00, 0x00, 0x00}), false) == "42  0x0000002A");
    REQUIRE(format_node_value_display(NodeKind::int32, bytes_of({0x2A, 0x00, 0x00, 0x00}), true) == "0x0000002A  42");
    REQUIRE(format_node_value_display(NodeKind::int8, bytes_of({0xFF}), false) == "-1  0xFF");
    REQUIRE(format_node_value_display(NodeKind::uint32, bytes_of({0xFF, 0xFF, 0xFF, 0xFF}), false)
            == "4294967295  0xFFFFFFFF");
    REQUIRE(format_node_value_display(NodeKind::uint16, bytes_of({0x10, 0x27}), true) == "0x2710  10000");

    // A float reads at three decimals, or as its bits when shown as hex.
    REQUIRE(format_node_value_display(NodeKind::float32, bytes_of({0xC3, 0xF5, 0x48, 0x40}), false) == "3.140");
    REQUIRE(format_node_value_display(NodeKind::float32, bytes_of({0xC3, 0xF5, 0x48, 0x40}), true) == "4048F5C3");
    REQUIRE(
        format_node_value_display(NodeKind::float64, bytes_of({0x1F, 0x85, 0xEB, 0x51, 0xB8, 0x1E, 0x09, 0x40}), false)
        == "3.140");

    // A hex node shows what its bytes would read as: the float, then the signed
    // value and the raw bits. A null one stops after the float.
    REQUIRE(format_node_value_display(NodeKind::hex32, bytes_of({0x2A, 0x00, 0x00, 0x00}), false)
            == "2A000000  0.000  42  0x2A");
    REQUIRE(
        format_node_value_display(NodeKind::hex64, bytes_of({0x40, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00}), false)
        == "4000400000000000  0.000  4194368  0x400040");
    REQUIRE(format_node_value_display(NodeKind::hex32, bytes_of({0x00, 0x00, 0x00, 0x00}), false)
            == "00000000  0.000  0");

    // The small kinds read as they always did.
    REQUIRE(format_node_value_display(NodeKind::hex8, bytes_of({0x0F}), false) == "0F");
    REQUIRE(format_node_value_display(NodeKind::hex16, bytes_of({0x0F, 0x10}), false) == "0F10");
    REQUIRE(format_node_value_display(NodeKind::boolean, bytes_of({0x01}), false) == "true");
    REQUIRE(format_node_value_display(NodeKind::character, bytes_of({0x41}), false) == "A");

    // A pointer is drawn as the address it holds.
    REQUIRE(format_node_value_display(NodeKind::pointer, bytes_of({0x40, 0x00, 0x40, 0x00, 0, 0, 0, 0}), false)
            == "-> 0x0000000000400040");
    REQUIRE(format_node_value_display(NodeKind::class_pointer, bytes_of({0, 0, 0, 0, 0, 0, 0, 0}), false)
            == "-> 0x0000000000000000");

    REQUIRE(format_node_value_display(NodeKind::int32, bytes_of({0x01, 0x02}), false).empty());
    REQUIRE(format_node_value_display(NodeKind::class_instance, bytes_of({0x01}), false).empty());

    // The value a hex node's bytes would read as, which the grid tests for an
    // address; a span that does not cover the kind has none.
    REQUIRE(read_node_value(NodeKind::hex64, bytes_of({0x40, 0x00, 0x40, 0x00, 0, 0, 0, 0})) == 0x400040);
    REQUIRE(read_node_value(NodeKind::hex8, bytes_of({0xFF})) == 0xFF);
    REQUIRE(read_node_value(NodeKind::hex64, bytes_of({0x01, 0x02})) == 0);
    REQUIRE(read_node_value(NodeKind::class_instance, bytes_of({0x01})) == 0);
}

TEST_CASE("malformed values are refused with a reason", "[reclass]")
{
    REQUIRE_FALSE(parse_node_value(NodeKind::int32, "").has_value());
    REQUIRE_FALSE(parse_node_value(NodeKind::int32, "twelve").has_value());
    REQUIRE_FALSE(parse_node_value(NodeKind::float64, "").has_value());

    const auto out_of_range = parse_node_value(NodeKind::int16, "99999");
    REQUIRE_FALSE(out_of_range.has_value());
    REQUIRE(out_of_range.error().find("Int16") != std::string::npos);
}
