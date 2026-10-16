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
    REQUIRE(format_node_value(NodeKind::hex32, bytes_of({0x11, 0x22, 0x33, 0x44}), false) == "11 22 33 44");
    REQUIRE(format_node_value(NodeKind::hex8, bytes_of({0x0C}), false) == "0C");

    REQUIRE(parsed(NodeKind::hex32, "11223344") == bytes_of({0x11, 0x22, 0x33, 0x44}));
    REQUIRE(parsed(NodeKind::hex32, "0x11223344") == bytes_of({0x11, 0x22, 0x33, 0x44}));
    REQUIRE(parsed(NodeKind::hex32, "11 22 33 44") == bytes_of({0x11, 0x22, 0x33, 0x44}));
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

    // An integer with the parenthesised hex alternative beside it, and the
    // other way round once the node is shown as hex.
    REQUIRE(format_node_value_display(NodeKind::int32, bytes_of({0x2A, 0x00, 0x00, 0x00}), false) == "42 (0x2A)");
    REQUIRE(format_node_value_display(NodeKind::int32, bytes_of({0x2A, 0x00, 0x00, 0x00}), true) == "0x2A (42)");
    REQUIRE(format_node_value_display(NodeKind::int8, bytes_of({0xFF}), false) == "-1 (0xFF)");
    REQUIRE(format_node_value_display(NodeKind::uint32, bytes_of({0xFF, 0xFF, 0xFF, 0xFF}), false)
            == "4294967295 (0xFFFFFFFF)");
    REQUIRE(format_node_value_display(NodeKind::uint16, bytes_of({0x10, 0x27}), true) == "0x2710 (10000)");

    // A float reads at three decimals, or as its bits when shown as hex.
    REQUIRE(format_node_value_display(NodeKind::float32, bytes_of({0xC3, 0xF5, 0x48, 0x40}), false) == "3.140");
    REQUIRE(format_node_value_display(NodeKind::float32, bytes_of({0xC3, 0xF5, 0x48, 0x40}), true) == "4048F5C3");
    REQUIRE(
        format_node_value_display(NodeKind::float64, bytes_of({0x1F, 0x85, 0xEB, 0x51, 0xB8, 0x1E, 0x09, 0x40}), false)
        == "3.140");

    // A hex node shows what its bytes would read as: the float, the signed
    // value and the raw bits, each in its own parentheses. A null cell draws
    // all three too.
    REQUIRE(format_node_value_display(NodeKind::hex32, bytes_of({0x2A, 0x00, 0x00, 0x00}), false)
            == "2A 00 00 00  (0.000) (42) (0x2A)");
    REQUIRE(
        format_node_value_display(NodeKind::hex64, bytes_of({0x40, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00}), false)
        == "40 00 40 00 00 00 00 00  (0.000) (4194368) (0x400040)");
    REQUIRE(format_node_value_display(NodeKind::hex32, bytes_of({0x00, 0x00, 0x00, 0x00}), false)
            == "00 00 00 00  (0.000) (0) (0x0)");

    // The small kinds read their bytes, their signed value and their raw bits.
    REQUIRE(format_node_value_display(NodeKind::hex8, bytes_of({0x0F}), false) == "0F  (15) (0xF)");
    REQUIRE(format_node_value_display(NodeKind::hex16, bytes_of({0x0F, 0x10}), false) == "0F 10  (4111) (0x100F)");
    REQUIRE(format_node_value_display(NodeKind::boolean, bytes_of({0x01}), false) == "true");
    REQUIRE(format_node_value_display(NodeKind::character, bytes_of({0x41}), false) == "A");

    // A pointer is drawn as the address it holds.
    REQUIRE(format_node_value_display(NodeKind::pointer, bytes_of({0x40, 0x00, 0x40, 0x00, 0, 0, 0, 0}), false)
            == "-> 400040");
    REQUIRE(format_node_value_display(NodeKind::class_pointer, bytes_of({0, 0, 0, 0, 0, 0, 0, 0}), false) == "-> 0");

    REQUIRE(format_node_value_display(NodeKind::int32, bytes_of({0x01, 0x02}), false).empty());
    REQUIRE(format_node_value_display(NodeKind::class_instance, bytes_of({0x01}), false).empty());

    // The value a hex node's bytes would read as, which the grid tests for an
    // address; a span that does not cover the kind has none.
    REQUIRE(read_node_value(NodeKind::hex64, bytes_of({0x40, 0x00, 0x40, 0x00, 0, 0, 0, 0})) == 0x400040);
    REQUIRE(read_node_value(NodeKind::hex8, bytes_of({0xFF})) == 0xFF);
    REQUIRE(read_node_value(NodeKind::hex64, bytes_of({0x01, 0x02})) == 0);
    REQUIRE(read_node_value(NodeKind::class_instance, bytes_of({0x01})) == 0);
}

TEST_CASE("the grid's hex read-outs are the shortest digits of their kind", "[reclass]")
{
    using slopkit::reclass::format_node_readings;
    using slopkit::reclass::format_node_value_display;

    // No kind pads its hex alternative: a wide kind built from a small value
    // reads as the digits it needs, not as its width.
    REQUIRE(format_node_value_display(NodeKind::int64, bytes_of({0x2A, 0, 0, 0, 0, 0, 0, 0}), false) == "42 (0x2A)");
    REQUIRE(format_node_value_display(NodeKind::uint64, bytes_of({0xFF, 0, 0, 0, 0, 0, 0, 0}), true) == "0xFF (255)");
    REQUIRE(format_node_value_display(NodeKind::uint8, bytes_of({0x00}), false) == "0 (0x0)");

    // A pointer draws the target's digits, and a hex cell's raw bits carry the
    // same `0x` prefix as an integer's hex alternative.
    REQUIRE(format_node_value_display(NodeKind::pointer, bytes_of({0x00, 0x00, 0x10, 0, 0, 0, 0, 0}), false)
            == "-> 100000");
    REQUIRE(format_node_readings(NodeKind::hex32, bytes_of({0x2A, 0x00, 0x00, 0x00})) == "(0.000) (42) (0x2A)");
}

TEST_CASE("a value's display text splits into byte and reading runs", "[reclass]")
{
    using slopkit::reclass::format_node_value_parts;

    // A hex32's dump is one run per byte, with the reading after them.
    const auto hex32 = format_node_value_parts(NodeKind::hex32, bytes_of({0x2A, 0x00, 0x00, 0x00}), false);
    REQUIRE(hex32.size() == 5);
    REQUIRE(hex32[0].text == "2A");
    REQUIRE(hex32[0].byte == 0);
    REQUIRE_FALSE(hex32[0].reading);
    REQUIRE(hex32[1].text == " 00");
    REQUIRE(hex32[1].byte == 1);
    REQUIRE(hex32[3].text == " 00");
    REQUIRE(hex32[3].byte == 3);
    REQUIRE(hex32[4].text == "  (0.000) (42) (0x2A)");
    REQUIRE(hex32[4].reading);
    REQUIRE_FALSE(hex32[4].byte.has_value());

    // The smaller hex kinds keep one run per byte and put their readings after
    // them: the signed value and the raw bits, no float.
    const auto hex8 = format_node_value_parts(NodeKind::hex8, bytes_of({0x1A}), false);
    REQUIRE(hex8.size() == 2);
    REQUIRE(hex8[0].text == "1A");
    REQUIRE(hex8[0].byte == 0);
    REQUIRE_FALSE(hex8[0].reading);
    REQUIRE(hex8[1].text == "  (26) (0x1A)");
    REQUIRE(hex8[1].reading);
    REQUIRE_FALSE(hex8[1].byte.has_value());

    const auto hex16 = format_node_value_parts(NodeKind::hex16, bytes_of({0x0F, 0x10}), false);
    REQUIRE(hex16.size() == 3);
    REQUIRE(hex16[0].text == "0F");
    REQUIRE(hex16[0].byte == 0);
    REQUIRE(hex16[1].text == " 10");
    REQUIRE(hex16[1].byte == 1);
    REQUIRE(hex16[2].text == "  (4111) (0x100F)");
    REQUIRE(hex16[2].reading);

    // A kind that draws no byte dump keeps one run with no byte.
    const auto integer = format_node_value_parts(NodeKind::int32, bytes_of({0x2A, 0x00, 0x00, 0x00}), false);
    REQUIRE(integer.size() == 1);
    REQUIRE(integer[0].text == "42 (0x2A)");
    REQUIRE_FALSE(integer[0].reading);
    REQUIRE_FALSE(integer[0].byte.has_value());

    // The fold is the display text.
    std::string folded;
    for (const auto& part : hex32)
    {
        folded += part.text;
    }
    REQUIRE(format_node_value_display(NodeKind::hex32, bytes_of({0x2A, 0x00, 0x00, 0x00}), false) == folded);
    REQUIRE(format_node_value_display(NodeKind::hex8, bytes_of({0x1A}), false) == "1A  (26) (0x1A)");
}

TEST_CASE("a hex node's readings are drawn one to a pair of parentheses", "[reclass]")
{
    using slopkit::reclass::format_node_reading;
    using slopkit::reclass::format_node_readings;
    using slopkit::reclass::node_readings;
    using slopkit::reclass::NodeReading;

    // The readings the grid shows beside a hex node's bytes, each in its own
    // parentheses, one space apart.
    REQUIRE(format_node_readings(NodeKind::hex32, bytes_of({0x2A, 0x00, 0x00, 0x00})) == "(0.000) (42) (0x2A)");
    REQUIRE(format_node_readings(NodeKind::hex64, bytes_of({0x40, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00}))
            == "(0.000) (4194368) (0x400040)");

    // Every reading is always drawn: a null cell spells out all three, a
    // negative value is signed, a large float still spells itself at three
    // decimals, and one that is not a number names itself.
    REQUIRE(format_node_readings(NodeKind::hex32, bytes_of({0x00, 0x00, 0x00, 0x00})) == "(0.000) (0) (0x0)");
    REQUIRE(format_node_readings(NodeKind::hex32, bytes_of({0xFF, 0xFF, 0xFF, 0xFF})) == "(NaN) (-1) (0xFFFFFFFF)");
    REQUIRE(format_node_readings(NodeKind::hex32, bytes_of({0x00, 0x00, 0x80, 0x4F}))
            == "(4294967296.000) (1333788672) (0x4F800000)");
    REQUIRE(format_node_readings(NodeKind::hex32, bytes_of({0x00, 0x00, 0x80, 0x7F}))
            == "(Inf) (2139095040) (0x7F800000)");
    REQUIRE(format_node_readings(NodeKind::hex32, bytes_of({0x00, 0x00, 0x80, 0xFF}))
            == "(-Inf) (-8388608) (0xFF800000)");

    // A hex8/hex16 cell has no float kind to spell its bytes as, so it draws its
    // signed value and its raw bits alone.
    REQUIRE(format_node_readings(NodeKind::hex8, bytes_of({0x7F})) == "(127) (0x7F)");
    REQUIRE(format_node_readings(NodeKind::hex8, bytes_of({0x80})) == "(-128) (0x80)");
    REQUIRE(format_node_readings(NodeKind::hex8, bytes_of({0x00})) == "(0) (0x0)");
    REQUIRE(format_node_readings(NodeKind::hex16, bytes_of({0xFF, 0xFF})) == "(-1) (0xFFFF)");

    // The readings in the order the hints field draws them, and each one on its
    // own.
    REQUIRE(node_readings(NodeKind::hex32)
            == std::vector<NodeReading> {NodeReading::floating, NodeReading::integer, NodeReading::raw});
    REQUIRE(node_readings(NodeKind::hex64)
            == std::vector<NodeReading> {NodeReading::floating, NodeReading::integer, NodeReading::raw});
    REQUIRE(node_readings(NodeKind::hex8) == std::vector<NodeReading> {NodeReading::integer, NodeReading::raw});
    REQUIRE(node_readings(NodeKind::hex16) == std::vector<NodeReading> {NodeReading::integer, NodeReading::raw});
    REQUIRE(node_readings(NodeKind::int32).empty());

    const auto bytes = bytes_of({0xFF, 0xFF, 0xFF, 0xFF});
    REQUIRE(format_node_reading(NodeReading::floating, NodeKind::hex32, bytes) == "NaN");
    REQUIRE(format_node_reading(NodeReading::integer, NodeKind::hex32, bytes) == "-1");
    REQUIRE(format_node_reading(NodeReading::raw, NodeKind::hex32, bytes) == "0xFFFFFFFF");

    // A reading the kind draws none of has no text: a small hex cell has no
    // float reading, and no kind outside the hex sizes has any.
    REQUIRE(format_node_readings(NodeKind::int32, bytes_of({0x2A, 0x00, 0x00, 0x00})).empty());
    REQUIRE(format_node_readings(NodeKind::hex64, bytes_of({0x01, 0x02})).empty());
    REQUIRE(format_node_readings(NodeKind::hex16, bytes_of({0x01})).empty());
    REQUIRE(format_node_reading(NodeReading::raw, NodeKind::int32, bytes_of({0x2A, 0x00, 0x00, 0x00})).empty());
    REQUIRE(format_node_reading(NodeReading::raw, NodeKind::hex64, bytes_of({0x01, 0x02})).empty());
    REQUIRE(format_node_reading(NodeReading::floating, NodeKind::hex8, bytes_of({0x7F})).empty());
    REQUIRE(format_node_reading(NodeReading::floating, NodeKind::hex16, bytes_of({0x01, 0x02})).empty());
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

TEST_CASE("a typed reading is written in the base that reading is drawn in", "[reclass]")
{
    using slopkit::reclass::NodeReading;
    using slopkit::reclass::parse_node_reading;

    const auto read = [](NodeReading reading, NodeKind kind, std::string_view text, bool hex = false)
    {
        const auto bytes = parse_node_reading(reading, kind, text, hex);
        REQUIRE(bytes.has_value());
        return *bytes;
    };

    // Each reading writes back the text it is drawn with: the integer's sign
    // extension, the raw bits' full width and the float's own spelling.
    REQUIRE(read(NodeReading::integer, NodeKind::hex32, "42") == bytes_of({0x2A, 0x00, 0x00, 0x00}));
    REQUIRE(read(NodeReading::integer, NodeKind::hex32, "-1") == bytes_of({0xFF, 0xFF, 0xFF, 0xFF}));
    REQUIRE(read(NodeReading::integer, NodeKind::hex64, "-9223372036854775808")
            == bytes_of({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80}));
    REQUIRE(read(NodeReading::raw, NodeKind::hex32, "0x2A") == bytes_of({0x2A, 0x00, 0x00, 0x00}));
    REQUIRE(read(NodeReading::raw, NodeKind::hex64, "0xFFFFFFFFFFFFFFFF")
            == bytes_of({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
    REQUIRE(read(NodeReading::floating, NodeKind::hex32, "3.14") == bytes_of({0xC3, 0xF5, 0x48, 0x40}));
    REQUIRE(read(NodeReading::floating, NodeKind::hex64, "3.14")
            == bytes_of({0x1F, 0x85, 0xEB, 0x51, 0xB8, 0x1E, 0x09, 0x40}));

    // 0x means hex everywhere, an integer's bare digits are decimal, and the
    // raw bits read bare digits as hex whatever the node's own toggle says.
    REQUIRE(read(NodeReading::integer, NodeKind::hex32, "0x2A") == read(NodeReading::integer, NodeKind::hex32, "42"));
    REQUIRE(read(NodeReading::integer, NodeKind::hex32, "0x30") == bytes_of({0x30, 0x00, 0x00, 0x00}));
    REQUIRE(read(NodeReading::integer, NodeKind::hex32, "30") == bytes_of({0x1E, 0x00, 0x00, 0x00}));
    for (const bool hex : {false, true})
    {
        REQUIRE(read(NodeReading::raw, NodeKind::hex32, "30", hex) == bytes_of({0x30, 0x00, 0x00, 0x00}));
    }

    // A float the bytes cannot spell is typed back as the drawn name of the
    // value, in any case.
    REQUIRE(read(NodeReading::floating, NodeKind::hex32, "NaN") == bytes_of({0x00, 0x00, 0xC0, 0x7F}));
    REQUIRE(read(NodeReading::floating, NodeKind::hex32, "nan") == bytes_of({0x00, 0x00, 0xC0, 0x7F}));
    REQUIRE(read(NodeReading::floating, NodeKind::hex32, "INF") == bytes_of({0x00, 0x00, 0x80, 0x7F}));
    REQUIRE(read(NodeReading::floating, NodeKind::hex32, "-Infinity") == bytes_of({0x00, 0x00, 0x80, 0xFF}));
    REQUIRE(read(NodeReading::floating, NodeKind::hex64, "Inf")
            == bytes_of({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x7F}));

    // What is not a reading's text, and what the node cannot hold, is refused.
    REQUIRE_FALSE(parse_node_reading(NodeReading::integer, NodeKind::hex32, "twelve").has_value());
    REQUIRE_FALSE(parse_node_reading(NodeReading::integer, NodeKind::hex32, "").has_value());
    REQUIRE_FALSE(parse_node_reading(NodeReading::raw, NodeKind::hex32, "0xZZ").has_value());
    REQUIRE_FALSE(parse_node_reading(NodeReading::raw, NodeKind::hex32, "-1").has_value());
    REQUIRE_FALSE(parse_node_reading(NodeReading::floating, NodeKind::hex32, "half").has_value());
    REQUIRE_FALSE(parse_node_reading(NodeReading::integer, NodeKind::hex32, "2147483648").has_value());
    REQUIRE_FALSE(parse_node_reading(NodeReading::integer, NodeKind::hex32, "-2147483649").has_value());
    REQUIRE_FALSE(parse_node_reading(NodeReading::integer, NodeKind::hex32, "0x1FFFFFFFF").has_value());
    REQUIRE_FALSE(parse_node_reading(NodeReading::floating, NodeKind::hex32, "1e40").has_value());

    const auto too_wide = parse_node_reading(NodeReading::raw, NodeKind::hex32, "0x1FFFFFFFF");
    REQUIRE_FALSE(too_wide.has_value());
    REQUIRE(too_wide.error().find("Hex32") != std::string::npos);
    REQUIRE_FALSE(parse_node_reading(NodeReading::raw, NodeKind::hex64, "0x1FFFFFFFFFFFFFFFF").has_value());

    // A hex8/hex16 cell reads its own integer and its raw bits, both of its own
    // width, and no float its width cannot spell.
    REQUIRE(read(NodeReading::integer, NodeKind::hex8, "-1") == bytes_of({0xFF}));
    REQUIRE(read(NodeReading::integer, NodeKind::hex16, "42") == bytes_of({0x2A, 0x00}));
    REQUIRE(read(NodeReading::raw, NodeKind::hex8, "FF") == bytes_of({0xFF}));
    REQUIRE(read(NodeReading::raw, NodeKind::hex16, "0xFFFF") == bytes_of({0xFF, 0xFF}));

    // A kind that draws no readings has none to write, and neither has a reading
    // the kind is without: a small hex cell has no float.
    REQUIRE_FALSE(parse_node_reading(NodeReading::raw, NodeKind::int32, "2A").has_value());
    REQUIRE_FALSE(parse_node_reading(NodeReading::floating, NodeKind::hex8, "1.5").has_value());
    REQUIRE_FALSE(parse_node_reading(NodeReading::floating, NodeKind::hex16, "1.5").has_value());

    // A value too wide for the small kinds is refused with the kind's own name.
    REQUIRE_FALSE(parse_node_reading(NodeReading::integer, NodeKind::hex8, "128").has_value());
    REQUIRE_FALSE(parse_node_reading(NodeReading::raw, NodeKind::hex8, "0x1FF").has_value());
    const auto wide_small = parse_node_reading(NodeReading::raw, NodeKind::hex16, "0x1FFFF");
    REQUIRE_FALSE(wide_small.has_value());
    REQUIRE(wide_small.error().find("Hex16") != std::string::npos);
}
