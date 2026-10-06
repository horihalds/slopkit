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
    CHECK(instruction.text == ".byte 48");

    CHECK(slopkit::disasm::decode_block(code, 0x2000, 10).empty());
}

TEST_CASE("an undecodable byte becomes a .byte row without stopping the listing", "[disasm]")
{
    const auto invalid = *slopkit::disasm::decode(bytes({0x06}), 0x3000);
    CHECK_FALSE(invalid.valid);
    CHECK(invalid.length == 1);
    CHECK(invalid.text == ".byte 06");

    const auto stream       = bytes({0xC3, 0x06, 0xC3});
    const auto instructions = slopkit::disasm::decode_block(stream, 0x3000, 10);
    REQUIRE(instructions.size() == 3);
    CHECK(instructions[0].text == "RET");
    CHECK_FALSE(instructions[1].valid);
    CHECK(instructions[1].text == ".byte 06");
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

TEST_CASE("a relative branch reports the address slice its text prints", "[disasm]")
{
    const auto instruction = *slopkit::disasm::decode(bytes({0x74, 0x15}), 0x2000);
    CHECK(instruction.text == "JZ 0000000000002017");
    REQUIRE(instruction.addresses.size() == 1);
    CHECK(instruction.addresses[0].offset == 3);
    CHECK(instruction.addresses[0].length == 16);
    CHECK(instruction.addresses[0].address == 0x2017);
    CHECK(instruction.text.substr(instruction.addresses[0].offset, instruction.addresses[0].length)
          == "0000000000002017");
}

TEST_CASE("32-bit relative branches report their address slice", "[disasm]")
{
    const auto jmp = *slopkit::disasm::decode(bytes({0xE9, 0x00, 0x00, 0x00, 0x00}), 0x1000);
    CHECK(jmp.text == "JMP 0000000000001005");
    REQUIRE(jmp.addresses.size() == 1);
    CHECK(jmp.addresses[0].address == 0x1005);
    CHECK(jmp.text.substr(jmp.addresses[0].offset, jmp.addresses[0].length) == "0000000000001005");

    const auto call = *slopkit::disasm::decode(bytes({0xE8, 0x10, 0x00, 0x00, 0x00}), 0x1000);
    CHECK(call.text == "CALL 0000000000001015");
    REQUIRE(call.addresses.size() == 1);
    CHECK(call.addresses[0].address == 0x1015);
}

TEST_CASE("a prefixed relative branch reports its address slice", "[disasm]")
{
    const auto instruction = *slopkit::disasm::decode(bytes({0x67, 0xE3, 0x12}), 0x1000);
    REQUIRE(instruction.addresses.size() == 1);
    CHECK(instruction.addresses[0].offset == 6);
    CHECK(instruction.addresses[0].length == 8);
    CHECK(instruction.addresses[0].address == 0x1015);
}

TEST_CASE("RIP-relative and indirect memory operands report their address slice", "[disasm]")
{
    const auto mov = *slopkit::disasm::decode(bytes({0x48, 0x8B, 0x05, 0xF7, 0x02, 0x00, 0x00}), 0x1000);
    CHECK(mov.text == "MOV RAX, [00000000000012FE]");
    REQUIRE(mov.addresses.size() == 1);
    CHECK(mov.addresses[0].address == 0x12FE);
    CHECK(mov.text.substr(mov.addresses[0].offset, mov.addresses[0].length) == "00000000000012FE");

    const auto call = *slopkit::disasm::decode(bytes({0xFF, 0x15, 0x0A, 0x00, 0x00, 0x00}), 0x1000);
    REQUIRE(call.addresses.size() == 1);
    CHECK(call.addresses[0].address == 0x1010);

    const auto push = *slopkit::disasm::decode(bytes({0xFF, 0x35, 0x0A, 0x00, 0x00, 0x00}), 0x1000);
    REQUIRE(push.addresses.size() == 1);
    CHECK(push.addresses[0].address == 0x1010);

    const auto lea = *slopkit::disasm::decode(bytes({0x48, 0x8D, 0x05, 0xF7, 0x02, 0x00, 0x00}), 0x1000);
    REQUIRE(lea.addresses.size() == 1);
    CHECK(lea.addresses[0].address == 0x12FE);
}

TEST_CASE("an absolute memory operand reports its address slice", "[disasm]")
{
    const auto instruction = *slopkit::disasm::decode(bytes({0x48, 0x8B, 0x04, 0x25, 0x00, 0x20, 0x40, 0x00}), 0x1000);
    REQUIRE(instruction.addresses.size() == 1);
    CHECK(instruction.addresses[0].address == 0x402000);
    CHECK(instruction.text.substr(instruction.addresses[0].offset, instruction.addresses[0].length)
          == "0000000000402000");
}

TEST_CASE("instructions without a printed address carry no slices", "[disasm]")
{
    CHECK(slopkit::disasm::decode(bytes({0x55}), 0x1000)->addresses.empty());             // PUSH RBP
    CHECK(slopkit::disasm::decode(bytes({0x48, 0x89, 0xD8}), 0x1000)->addresses.empty()); // MOV RAX, RBX
    CHECK(slopkit::disasm::decode(bytes({0xC3}), 0x1000)->addresses.empty());             // RET

    // MOV RAX, 402000 - an immediate that looks like an address, but is a value.
    CHECK(slopkit::disasm::decode(bytes({0x48, 0xB8, 0x00, 0x20, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00}), 0x1000)
              ->addresses.empty());

    const auto dot_byte = *slopkit::disasm::decode(bytes({0x06}), 0x1000);
    CHECK_FALSE(dot_byte.valid);
    CHECK(dot_byte.addresses.empty());

    const auto truncated = *slopkit::disasm::decode(bytes({0x48, 0x89}), 0x1000);
    CHECK_FALSE(truncated.valid);
    CHECK(truncated.addresses.empty());
}

TEST_CASE("decode_block keeps every slice inside its own instruction", "[disasm]")
{
    const auto code         = bytes({0x74, 0x15, 0x48, 0x8B, 0x05, 0xF7, 0x02, 0x00, 0x00, 0xC3});
    const auto instructions = slopkit::disasm::decode_block(code, 0x2000, 10);

    REQUIRE(instructions.size() == 3);
    REQUIRE(instructions[0].addresses.size() == 1);
    CHECK(instructions[0].addresses[0].address == 0x2017);

    REQUIRE(instructions[1].addresses.size() == 1);
    CHECK(instructions[1].address == 0x2002);
    CHECK(instructions[1].addresses[0].address == 0x2002 + 7 + 0x2F7);

    CHECK(instructions[2].addresses.empty());

    for (const auto& instruction : instructions)
    {
        for (const auto& ref : instruction.addresses)
        {
            CHECK(ref.offset + ref.length <= instruction.text.size());
        }
    }
}

TEST_CASE("a register-relative store describes the memory it writes", "[disasm]")
{
    const auto instruction = *slopkit::disasm::decode(bytes({0x89, 0x43, 0x08}), 0x1000);
    CHECK(instruction.text == "MOV [RBX+08], EAX");
    REQUIRE(instruction.memory.size() == 1);

    const auto& ref = instruction.memory[0];
    CHECK(ref.base == "RBX");
    CHECK(ref.index.empty());
    CHECK(ref.scale == 1);
    CHECK(ref.displacement == 8);
    CHECK(ref.width == 4);
    CHECK_FALSE(ref.rip_relative);
    CHECK(ref.writes);
    CHECK(instruction.text.substr(ref.offset, ref.length) == "[RBX+08]");
}

TEST_CASE("a scaled index operand reports base, index, scale and displacement", "[disasm]")
{
    const auto instruction = *slopkit::disasm::decode(bytes({0x48, 0x8B, 0x44, 0x8B, 0x10}), 0x1000);
    CHECK(instruction.text == "MOV RAX, [RBX+RCX*4+10]");
    REQUIRE(instruction.memory.size() == 1);

    const auto& ref = instruction.memory[0];
    CHECK(ref.base == "RBX");
    CHECK(ref.index == "RCX");
    CHECK(ref.scale == 4);
    CHECK(ref.displacement == 16);
    CHECK(ref.width == 8);
    CHECK_FALSE(ref.writes);
    CHECK(instruction.text.substr(ref.offset, ref.length) == "[RBX+RCX*4+10]");
}

TEST_CASE("a rip-relative load is flagged and keeps its displacement", "[disasm]")
{
    const auto instruction = *slopkit::disasm::decode(bytes({0x48, 0x8B, 0x05, 0x34, 0x12, 0x00, 0x00}), 0x1000);
    CHECK(instruction.text == "MOV RAX, [000000000000223B]");
    REQUIRE(instruction.memory.size() == 1);

    const auto& ref = instruction.memory[0];
    CHECK(ref.rip_relative);
    CHECK(ref.base == "RIP");
    CHECK(ref.index.empty());
    CHECK(ref.displacement == 0x1234);
    CHECK(ref.width == 8);
    CHECK_FALSE(ref.writes);
}

TEST_CASE("an absolute memory operand has no base or index", "[disasm]")
{
    const auto instruction = *slopkit::disasm::decode(bytes({0x48, 0x8B, 0x04, 0x25, 0x00, 0x20, 0x40, 0x00}), 0x1000);
    REQUIRE(instruction.memory.size() == 1);

    const auto& ref = instruction.memory[0];
    CHECK(ref.base.empty());
    CHECK(ref.index.empty());
    CHECK_FALSE(ref.rip_relative);
    CHECK(ref.displacement == 0x402000);
    CHECK(ref.width == 8);
}

TEST_CASE("lea and .byte rows carry no memory operand", "[disasm]")
{
    const auto lea = *slopkit::disasm::decode(bytes({0x48, 0x8D, 0x05, 0xF7, 0x02, 0x00, 0x00}), 0x1000);
    CHECK(lea.memory.empty());

    const auto dot_byte = *slopkit::disasm::decode(bytes({0x06}), 0x1000);
    CHECK(dot_byte.memory.empty());
}
