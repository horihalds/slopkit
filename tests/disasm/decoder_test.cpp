#include <catch2/catch.hpp>

#include <cstddef>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

#include "disasm/decoder.hpp"

namespace
{
    using slopkit::disasm::MachineMode;

    [[nodiscard]] std::vector<std::byte> bytes(std::initializer_list<unsigned> values)
    {
        std::vector<std::byte> result;
        result.reserve(values.size());
        for (const unsigned value : values)
        {
            result.push_back(static_cast<std::byte>(value));
        }
        return result;
    }
} // namespace

TEST_CASE("known encodings decode to the expected instruction text", "[disasm]")
{
    const auto push = bytes({0x55});
    REQUIRE(slopkit::disasm::decode(push, 0x1000).has_value());
    CHECK(slopkit::disasm::decode(push, 0x1000)->text == "PUSH RBP");

    const auto mov = bytes({0x48, 0x89, 0xE5});
    REQUIRE(slopkit::disasm::decode(mov, 0x1000).has_value());
    CHECK(slopkit::disasm::decode(mov, 0x1000)->text == "MOV RBP, RSP");

    const auto ret = bytes({0xC3});
    REQUIRE(slopkit::disasm::decode(ret, 0x1000).has_value());
    CHECK(slopkit::disasm::decode(ret, 0x1000)->text == "RET");

    const auto syscall = bytes({0x0F, 0x05});
    REQUIRE(slopkit::disasm::decode(syscall, 0x1000).has_value());
    CHECK(slopkit::disasm::decode(syscall, 0x1000)->text == "SYSCALL");
}

TEST_CASE("decode reports the instruction length and address", "[disasm]")
{
    const auto mov         = bytes({0x48, 0x89, 0xE5});
    const auto instruction = *slopkit::disasm::decode(mov, 0x4000);
    CHECK(instruction.address == 0x4000);
    CHECK(instruction.length == 3);
    CHECK(instruction.valid);
}

TEST_CASE("decode_block advances the address by each instruction length", "[disasm]")
{
    const auto code         = bytes({0x55, 0x48, 0x89, 0xE5, 0xC3});
    const auto instructions = slopkit::disasm::decode_block(code, 0x2000, 10);

    REQUIRE(instructions.size() == 3);
    CHECK(instructions[0].text == "PUSH RBP");
    CHECK(instructions[0].address == 0x2000);
    CHECK(instructions[1].text == "MOV RBP, RSP");
    CHECK(instructions[1].address == 0x2001);
    CHECK(instructions[2].text == "RET");
    CHECK(instructions[2].address == 0x2004);
}

TEST_CASE("decode_block honours the requested count", "[disasm]")
{
    const auto code = bytes({0x55, 0x55, 0x55});
    CHECK(slopkit::disasm::decode_block(code, 0x2000, 2).size() == 2);
}

TEST_CASE("a truncated tail decodes to a .byte row and stops a block sweep", "[disasm]")
{
    const auto code        = bytes({0x48, 0x89});
    const auto instruction = *slopkit::disasm::decode(code, 0x2000);
    CHECK_FALSE(instruction.valid);
    CHECK(instruction.length == 1);
    CHECK(instruction.text == ".byte 0x48");

    CHECK(slopkit::disasm::decode_block(code, 0x2000, 10).empty());
}

TEST_CASE("an undecodable byte becomes a .byte row without stopping the listing", "[disasm]")
{
    const auto invalid = *slopkit::disasm::decode(bytes({0x06}), 0x3000);
    CHECK_FALSE(invalid.valid);
    CHECK(invalid.length == 1);
    CHECK(invalid.text == ".byte 0x06");

    const auto stream       = bytes({0xC3, 0x06, 0xC3});
    const auto instructions = slopkit::disasm::decode_block(stream, 0x3000, 10);
    REQUIRE(instructions.size() == 3);
    CHECK(instructions[0].text == "RET");
    CHECK_FALSE(instructions[1].valid);
    CHECK(instructions[1].text == ".byte 0x06");
    CHECK(instructions[2].text == "RET");
}

TEST_CASE("the machine mode changes how the same bytes decode", "[disasm]")
{
    const auto code = bytes({0x48});

    const auto sixty_four = *slopkit::disasm::decode(code, 0x1000, MachineMode::long_64);
    CHECK_FALSE(sixty_four.valid);

    const auto thirty_two = *slopkit::disasm::decode(code, 0x1000, MachineMode::legacy_32);
    CHECK(thirty_two.valid);
    CHECK(thirty_two.text == "DEC EAX");
}

TEST_CASE("an empty buffer decodes to nothing", "[disasm]")
{
    CHECK_FALSE(slopkit::disasm::decode({}, 0x1000).has_value());
    CHECK(slopkit::disasm::decode_block({}, 0x1000, 10).empty());
}
