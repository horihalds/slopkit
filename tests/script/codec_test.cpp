#include <catch2/catch.hpp>

#include <cstddef>
#include <initializer_list>
#include <vector>

#include "script/codec.hpp"

namespace
{
    using slopkit::script::decode_number;
    using slopkit::script::encode_number;
    using slopkit::script::type_width;

    std::vector<std::byte> bytes_of(std::initializer_list<int> values)
    {
        std::vector<std::byte> bytes;
        for (const int value : values)
        {
            bytes.push_back(static_cast<std::byte>(value));
        }
        return bytes;
    }
} // namespace

TEST_CASE("script codec maps every token to its width", "[script]")
{
    CHECK(type_width("u8") == 1);
    CHECK(type_width("i8") == 1);
    CHECK(type_width("u16") == 2);
    CHECK(type_width("i16") == 2);
    CHECK(type_width("u32") == 4);
    CHECK(type_width("i32") == 4);
    CHECK(type_width("u64") == 8);
    CHECK(type_width("i64") == 8);
    CHECK(type_width("f32") == 4);
    CHECK(type_width("f64") == 8);
    CHECK(type_width("ptr") == 8);

    CHECK_FALSE(type_width("").has_value());
    CHECK_FALSE(type_width("u24").has_value());
    CHECK_FALSE(type_width("U32").has_value());
}

TEST_CASE("script codec decodes integers little-endian", "[script]")
{
    CHECK(*decode_number("u8", bytes_of({0xFF})) == 255.0);
    CHECK(*decode_number("i8", bytes_of({0xFF})) == -1.0);
    CHECK(*decode_number("u16", bytes_of({0x34, 0x12})) == 0x1234);
    CHECK(*decode_number("i16", bytes_of({0xFF, 0xFF})) == -1.0);
    CHECK(*decode_number("u32", bytes_of({0x04, 0x03, 0x02, 0x01})) == 0x01020304);
    CHECK(*decode_number("i32", bytes_of({0xFF, 0xFF, 0xFF, 0xFF})) == -1.0);
    CHECK(*decode_number("i32", bytes_of({0x00, 0x00, 0x00, 0x80})) == -2147483648.0);
    CHECK(*decode_number("u64", bytes_of({0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00})) == 255.0);
    CHECK(*decode_number("ptr", bytes_of({0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00})) == 4096.0);
}

TEST_CASE("script codec decodes floats", "[script]")
{
    CHECK(*decode_number("f32", bytes_of({0x00, 0x00, 0x80, 0x3F})) == 1.0);
    CHECK(*decode_number("f64", bytes_of({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F})) == 1.0);
    CHECK(*decode_number("f32", bytes_of({0x00, 0x00, 0xC0, 0x3F})) == 1.5);
}

TEST_CASE("script codec encodes what it decodes", "[script]")
{
    for (const char* token : {"u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64", "ptr"})
    {
        const auto encoded = encode_number(token, 0x12);
        REQUIRE(encoded.has_value());
        CHECK(*decode_number(token, *encoded) == 0x12);
    }

    const auto negative = encode_number("i32", -2.0);
    REQUIRE(negative.has_value());
    CHECK(negative->size() == 4);
    CHECK(*decode_number("i32", *negative) == -2.0);

    const auto unsigned_wrap = encode_number("u32", -1.0);
    REQUIRE(unsigned_wrap.has_value());
    CHECK(*decode_number("u32", *unsigned_wrap) == 4294967295.0);

    for (const char* token : {"f32", "f64"})
    {
        const auto encoded = encode_number(token, 2.5);
        REQUIRE(encoded.has_value());
        CHECK(*decode_number(token, *encoded) == 2.5);
    }
}

TEST_CASE("script codec rejects unknown tokens and wrong widths", "[script]")
{
    CHECK_FALSE(decode_number("u24", bytes_of({0x00, 0x00, 0x00})).has_value());
    CHECK_FALSE(decode_number("u32", bytes_of({0x00, 0x00, 0x00})).has_value());
    CHECK_FALSE(decode_number("u8", bytes_of({0x00, 0x00})).has_value());
    CHECK_FALSE(encode_number("nope", 1.0).has_value());

    CHECK(decode_number("u24", bytes_of({0x00, 0x00, 0x00})).error() == "unknown value type 'u24'");
}
