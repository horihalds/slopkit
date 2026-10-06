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
