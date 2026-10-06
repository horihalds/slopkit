#include <catch2/catch.hpp>

#include "support/scan_helpers.hpp"

TEST_CASE("value literals parse for every value type", "[scan]")
{
    CHECK(std::get<std::int64_t>(*slopkit::scan::parse_value(ValueType::byte, "42", false)) == 42);
    CHECK(std::get<std::int64_t>(*slopkit::scan::parse_value(ValueType::byte, "-1", false)) == -1);
    CHECK(std::get<std::int64_t>(*slopkit::scan::parse_value(ValueType::byte, "0x80", false)) == -128);
    CHECK(std::get<std::int64_t>(*slopkit::scan::parse_value(ValueType::int16, "0x1234", false)) == 0x1234);
    CHECK(std::get<std::int64_t>(*slopkit::scan::parse_value(ValueType::int32, "0xFFFFFFFF", false)) == -1);
    CHECK(std::get<std::int64_t>(*slopkit::scan::parse_value(ValueType::int64, "0xFFFFFFFFFFFFFFFF", false)) == -1);
    CHECK(std::get<double>(*slopkit::scan::parse_value(ValueType::float64, "3.5", false)) == 3.5);
    CHECK(std::get<double>(*slopkit::scan::parse_value(ValueType::float32, "-2.25", false)) == -2.25);
    CHECK(std::get<std::string>(*slopkit::scan::parse_value(ValueType::string, "hello", false)) == "hello");

    const auto bytes =
        std::get<std::vector<std::byte>>(*slopkit::scan::parse_value(ValueType::byte_array, "DE AD 01", true));
    REQUIRE(bytes.size() == 3);
    CHECK(static_cast<unsigned>(bytes[0]) == 0xDE);
    CHECK(static_cast<unsigned>(bytes[1]) == 0xAD);
    CHECK(static_cast<unsigned>(bytes[2]) == 0x01);
}

TEST_CASE("malformed values are reported instead of throwing", "[scan]")
{
    CHECK_FALSE(slopkit::scan::parse_value(ValueType::int32, "abc", false).has_value());
    CHECK_FALSE(slopkit::scan::parse_value(ValueType::int32, "", false).has_value());
    CHECK_FALSE(slopkit::scan::parse_value(ValueType::byte, "128", false).has_value());
    CHECK_FALSE(slopkit::scan::parse_value(ValueType::int32, "1e3", false).has_value());
    CHECK_FALSE(slopkit::scan::parse_value(ValueType::float64, "0x10", true).has_value());
    CHECK_FALSE(slopkit::scan::parse_value(ValueType::byte_array, "300", false).has_value());
}

TEST_CASE("values format back to text", "[scan]")
{
    std::vector<std::byte> bytes(4);
    write_int32(bytes, 0, 0x01020304);

    CHECK(slopkit::scan::format_value(ValueType::int32, bytes, false) == "16909060");
    CHECK(slopkit::scan::format_value(ValueType::int32, bytes, true) == "01020304");

    std::vector<std::byte> negative(4);
    write_int32(negative, 0, -1);
    CHECK(slopkit::scan::format_value(ValueType::int32, negative, false) == "-1");
    CHECK(slopkit::scan::format_value(ValueType::int32, negative, true) == "FFFFFFFF");
}

TEST_CASE("editable values convert between bases", "[scan]")
{
    SECTION("an integer reads decimal and renders bare hex, and back")
    {
        CHECK(slopkit::scan::convert_value_base(ValueType::int32, "10", false, true) == "A");
        CHECK(slopkit::scan::convert_value_base(ValueType::int32, "A", true, false) == "10");
    }

    SECTION("a byte masks to its own width")
    {
        CHECK(slopkit::scan::convert_value_base(ValueType::byte, "127", false, true) == "7F");
        CHECK(slopkit::scan::convert_value_base(ValueType::byte, "7F", true, false) == "127");
        CHECK(slopkit::scan::convert_value_base(ValueType::byte, "-1", false, true) == "FF");
        CHECK(slopkit::scan::convert_value_base(ValueType::byte, "FF", true, false) == "-1");
    }

    SECTION("a negative int64 round-trips through its full width")
    {
        const auto hex = slopkit::scan::convert_value_base(ValueType::int64, "-1", false, true);
        REQUIRE(hex.has_value());
        CHECK(*hex == "FFFFFFFFFFFFFFFF");
        CHECK(slopkit::scan::convert_value_base(ValueType::int64, *hex, true, false) == "-1");
    }

    SECTION("a byte array converts token by token")
    {
        CHECK(slopkit::scan::convert_value_base(ValueType::byte_array, "10 20", false, true) == "A 14");
        CHECK(slopkit::scan::convert_value_base(ValueType::byte_array, "A 14", true, false) == "10 20");
    }

    SECTION("an explicit prefix wins over the current base")
    {
        CHECK(slopkit::scan::convert_value_base(ValueType::int32, "0x10", false, true) == "10");
        CHECK(slopkit::scan::convert_value_base(ValueType::int32, "#16", true, false) == "16");
    }

    SECTION("text with no counterpart is left alone")
    {
        CHECK_FALSE(slopkit::scan::convert_value_base(ValueType::int32, "", false, true).has_value());
        CHECK_FALSE(slopkit::scan::convert_value_base(ValueType::int32, "12Z", false, true).has_value());
        CHECK_FALSE(slopkit::scan::convert_value_base(ValueType::byte, "300", false, true).has_value());
        CHECK_FALSE(slopkit::scan::convert_value_base(ValueType::float32, "1.5", false, true).has_value());
        CHECK_FALSE(slopkit::scan::convert_value_base(ValueType::float64, "1.5", false, true).has_value());
        CHECK_FALSE(slopkit::scan::convert_value_base(ValueType::string, "hello", false, true).has_value());
    }
}
