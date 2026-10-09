#include <catch2/catch.hpp>

#include <algorithm>
#include <cstddef>
#include <string>

#include "support/script_helpers.hpp"

TEST_CASE("script engine reads typed memory with the read_* globals", "[script]")
{
    ScriptFixture fixture(0x100);
    fixture.fake.put(0x00, {0x01, 0x02, 0x03, 0x04});
    fixture.fake.put(0x10, {0xFF});                                           // u8 / i8
    fixture.fake.put(0x11, {0xFF, 0xFF});                                     // u16 / i16
    fixture.fake.put(0x13, {0xFF, 0xFF, 0xFF, 0xFF});                         // u32 / i32
    fixture.fake.put(0x17, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}); // i64
    fixture.fake.put(0x20, {0x00, 0x00, 0x80, 0x3F});                         // f32 1.0
    fixture.fake.put(0x30, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F}); // f64 1.0

    const RunResult result = fixture.engine.run(R"(
print(read_u8(4096))
print(read_u16(4096))
print(read_u32(4096))
print(read_u8(4096 + 0x10))
print(read_i8(4096 + 0x10))
print(read_u16(4096 + 0x11))
print(read_i16(4096 + 0x11))
print(read_u32(4096 + 0x13))
print(read_i32(4096 + 0x13))
print(read_i64(4096 + 0x17))
print(read_f32(4096 + 0x20))
print(read_f64(4096 + 0x30))
)");

    REQUIRE(result.ok);
    REQUIRE(result.output.size() == 12);
    CHECK(result.output[0] == "1");
    CHECK(result.output[1] == "513");
    CHECK(result.output[2] == "67305985");
    CHECK(result.output[3] == "255");
    CHECK(result.output[4] == "-1");
    CHECK(result.output[5] == "65535");
    CHECK(result.output[6] == "-1");
    CHECK(result.output[7] == "4294967295");
    CHECK(result.output[8] == "-1");
    CHECK(result.output[9] == "-1");
    CHECK(result.output[10] == "1.0");
    CHECK(result.output[11] == "1.0");
}

TEST_CASE("write infers the narrowest width and lets a token override it", "[script]")
{
    ScriptFixture fixture;
    std::fill(fixture.fake.bytes.begin(), fixture.fake.bytes.end(), std::byte {0xAA});

    const RunResult result = fixture.engine.run(R"(
write(4096 + 0x00, 0)
write(4096 + 0x10, 255)
write(4096 + 0x20, 256)
write(4096 + 0x30, 65536)
write(4096 + 0x40, 4294967296)
write(4096 + 0x50, -1)
write(4096 + 0x60, -129)
write(4096 + 0x70, 1.5)
write(4096 + 0x80, 5, "u32")
write(4096 + 0x90, 1.5, "f64")
)");

    REQUIRE(result.ok);

    const auto byte_at = [&fixture](std::size_t offset)
    {
        return std::to_integer<unsigned>(fixture.fake.bytes[offset]);
    };

    CHECK(byte_at(0x00) == 0x00);
    CHECK(byte_at(0x01) == 0xAA); // one byte written

    CHECK(byte_at(0x10) == 0xFF);
    CHECK(byte_at(0x11) == 0xAA);

    CHECK(byte_at(0x20) == 0x00);
    CHECK(byte_at(0x21) == 0x01);
    CHECK(byte_at(0x22) == 0xAA); // two bytes written

    CHECK(byte_at(0x30) == 0x00);
    CHECK(byte_at(0x31) == 0x00);
    CHECK(byte_at(0x32) == 0x01);
    CHECK(byte_at(0x33) == 0x00);
    CHECK(byte_at(0x34) == 0xAA); // four bytes written

    CHECK(byte_at(0x40) == 0x00);
    CHECK(byte_at(0x44) == 0x01);
    CHECK(byte_at(0x47) == 0x00);
    CHECK(byte_at(0x48) == 0xAA); // eight bytes written

    CHECK(byte_at(0x50) == 0xFF);
    CHECK(byte_at(0x51) == 0xAA); // signed one byte

    CHECK(byte_at(0x60) == 0x7F);
    CHECK(byte_at(0x61) == 0xFF);
    CHECK(byte_at(0x62) == 0xAA); // signed two bytes

    CHECK(byte_at(0x70) == 0x00);
    CHECK(byte_at(0x71) == 0x00);
    CHECK(byte_at(0x72) == 0xC0);
    CHECK(byte_at(0x73) == 0x3F);
    CHECK(byte_at(0x74) == 0xAA); // f32

    CHECK(byte_at(0x80) == 0x05);
    CHECK(byte_at(0x83) == 0x00);
    CHECK(byte_at(0x84) == 0xAA); // explicit u32

    CHECK(byte_at(0x90) == 0x00);
    CHECK(byte_at(0x97) == 0x3F);
    CHECK(byte_at(0x98) == 0xAA); // explicit f64
}

TEST_CASE("write and the typed reads round-trip through the seam", "[script]")
{
    ScriptFixture fixture;

    const RunResult result = fixture.engine.run(R"(
write(4096 + 4, 0x1234, "u32")
print(read_u32(4096 + 4))
print(read_u16(4096 + 4))
print(read_u8(4096 + 4))
write(4096 + 8, -2, "i64")
print(read_i64(4096 + 8))
write(4096 + 16, 2.5, "f64")
print(read_f64(4096 + 16))
write(4096 + 24, 2.5)
print(read_f32(4096 + 24))
)");

    REQUIRE(result.ok);
    REQUIRE(result.output.size() == 6);
    CHECK(result.output[0] == "4660");
    CHECK(result.output[1] == "4660");
    CHECK(result.output[2] == "52");
    CHECK(result.output[3] == "-2");
    CHECK(result.output[4] == "2.5");
    CHECK(result.output[5] == "2.5");
}

TEST_CASE("the typed globals report failures under their own name", "[script]")
{
    ScriptFixture fixture;

    SECTION("an unmapped read")
    {
        const RunResult result = fixture.engine.run("read_u32(0)");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("read_u32") != std::string::npos);
        CHECK(result.error.find("address is not mapped") != std::string::npos);
    }

    SECTION("an unmapped write")
    {
        const RunResult result = fixture.engine.run("write(0, 1)");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("write") != std::string::npos);
        CHECK(result.error.find("address is not mapped") != std::string::npos);
    }

    SECTION("an unknown explicit token")
    {
        const RunResult result = fixture.engine.run("write(4096, 1, 'u24')");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("unknown value type 'u24'") != std::string::npos);
    }

    SECTION("a non-number value")
    {
        const RunResult result = fixture.engine.run("write(4096, 'x')");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("the value must be a number") != std::string::npos);
    }

    SECTION("a non-string token")
    {
        const RunResult result = fixture.engine.run("write(4096, 1, 4)");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("the value type must be a string") != std::string::npos);
    }
}
