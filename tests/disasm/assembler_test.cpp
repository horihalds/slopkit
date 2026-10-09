#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "disasm/assembler.hpp"
#include "disasm/decoder.hpp"

namespace
{
    using slopkit::disasm::AssembleBlockContext;
    using slopkit::disasm::AssembleContext;
    using slopkit::disasm::MachineMode;
    using slopkit::disasm::MemoryRef;

    constexpr std::uint64_t kAddress = 0x2000;

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

    [[nodiscard]] std::vector<std::byte>
    encoded(std::string_view text, std::uint64_t address = kAddress, std::span<const MemoryRef> memory = {})
    {
        const auto assembled = slopkit::disasm::assemble(
            text, AssembleContext {.address = address, .mode = MachineMode::long_64, .memory = memory});
        REQUIRE(assembled.has_value());
        return *assembled;
    }

    [[nodiscard]] std::vector<std::byte>
    block(std::string_view text, std::uint64_t base = kAddress, MachineMode mode = MachineMode::long_64)
    {
        const auto assembled = slopkit::disasm::assemble_block(text, AssembleBlockContext {.base = base, .mode = mode});
        REQUIRE(assembled.has_value());
        return *assembled;
    }

    // Assembles the text the listing prints for `code` and returns both, so a
    // case can compare them.
    struct RoundTrip
    {
        std::vector<std::byte> original;
        std::vector<std::byte> assembled;
    };

    [[nodiscard]] RoundTrip round_trip(std::initializer_list<unsigned> code, std::uint64_t address = kAddress)
    {
        RoundTrip result;
        result.original        = bytes(code);
        const auto instruction = slopkit::disasm::decode(result.original, address);
        REQUIRE(instruction.has_value());
        const auto assembled = slopkit::disasm::assemble(
            instruction->text,
            AssembleContext {.address = address, .mode = MachineMode::long_64, .memory = instruction->memory});
        REQUIRE(assembled.has_value());
        result.assembled = *assembled;
        return result;
    }
} // namespace

TEST_CASE("a register instruction assembles to the known encoding", "[disasm]")
{
    CHECK(encoded("MOV RBP, RSP") == bytes({0x48, 0x89, 0xE5}));
    CHECK(encoded("mov rbp, rsp") == bytes({0x48, 0x89, 0xE5}));
    CHECK(encoded("RET") == bytes({0xC3}));
    CHECK(encoded("NOP") == bytes({0x90}));
}

TEST_CASE("an immediate assembles in its shortest form", "[disasm]")
{
    CHECK(encoded("MOV EAX, 1") == bytes({0xB8, 0x01, 0x00, 0x00, 0x00}));
    CHECK(encoded("MOV RAX, 1") == bytes({0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00}));
    CHECK(encoded("SUB RSP, 0x20") == bytes({0x48, 0x83, 0xEC, 0x20}));
}

TEST_CASE("a bare number is hex and `#` selects decimal", "[disasm]")
{
    CHECK(encoded("MOV EAX, 10") == bytes({0xB8, 0x10, 0x00, 0x00, 0x00}));
    CHECK(encoded("MOV EAX, #10") == bytes({0xB8, 0x0A, 0x00, 0x00, 0x00}));
    CHECK(encoded("MOV EAX, 0x10") == bytes({0xB8, 0x10, 0x00, 0x00, 0x00}));
}

TEST_CASE("branch targets are assembled as absolute addresses", "[disasm]")
{
    CHECK(encoded("JZ 0x2017") == bytes({0x74, 0x15}));
    CHECK(encoded("CALL 0x2017") == bytes({0xE8, 0x12, 0x00, 0x00, 0x00}));
}

TEST_CASE("an out-of-reach branch target names the reach", "[disasm]")
{
    const auto jump = slopkit::disasm::assemble("JMP 0x7F0000000000", AssembleContext {.address = kAddress});
    REQUIRE_FALSE(jump.has_value());
    CHECK(jump.error() == "the encoder rejected 'JMP 0x7F0000000000' (a relative branch reaches at most ±2 GB)");

    const auto call = slopkit::disasm::assemble("CALL 0x7F0000000000", AssembleContext {.address = kAddress});
    REQUIRE_FALSE(call.has_value());
    CHECK(call.error() == "the encoder rejected 'CALL 0x7F0000000000' (a relative branch reaches at most ±2 GB)");

    // A register branch has no reach to exceed and still encodes.
    CHECK(encoded("JMP RAX") == bytes({0xFF, 0xE0}));
}

TEST_CASE("memory operands assemble with base, index and displacement", "[disasm]")
{
    CHECK(encoded("MOV RAX, [RBP+0x10]") == bytes({0x48, 0x8B, 0x45, 0x10}));
    CHECK(encoded("MOV [RAX+RCX*4], RBX") == bytes({0x48, 0x89, 0x1C, 0x88}));
    CHECK(encoded("MOV byte ptr [RBP-4], 1") == bytes({0xC6, 0x45, 0xFC, 0x01}));
}

TEST_CASE("unknown text is reported instead of a wrong instruction", "[disasm]")
{
    CHECK_FALSE(slopkit::disasm::assemble("", AssembleContext {.address = kAddress}).has_value());
    CHECK_FALSE(slopkit::disasm::assemble("FROBNICATE RAX, RBX", AssembleContext {.address = kAddress}).has_value());
    CHECK_FALSE(slopkit::disasm::assemble("MOV RAX, [RBX", AssembleContext {.address = kAddress}).has_value());
    CHECK_FALSE(slopkit::disasm::assemble("MOV RAX, [RIP+0x10]", AssembleContext {.address = kAddress}).has_value());
}

TEST_CASE("re-assembling a decoded instruction reproduces its bytes", "[disasm]")
{
    for (const auto& candidate : {round_trip({0x55}),
                                  round_trip({0x48, 0x89, 0xE5}),
                                  round_trip({0xC3}),
                                  round_trip({0x90}),
                                  // The listing text of an immediate and a displacement assembles back.
                                  round_trip({0xB8, 0x2A, 0x00, 0x00, 0x00}),
                                  round_trip({0x48, 0x8B, 0x45, 0x10})})
    {
        CHECK(candidate.assembled == candidate.original);
    }
}

TEST_CASE("a rip-relative memory operand stays rip-relative", "[disasm]")
{
    // MOV RAX, [RIP+0x2F7] and MOV byte ptr [RIP+0x2F7], 1: their text prints
    // the absolute target, and the context keeps them relative.
    const auto load = round_trip({0x48, 0x8B, 0x05, 0xF7, 0x02, 0x00, 0x00});
    CHECK(load.assembled == load.original);

    const auto store = round_trip({0xC6, 0x05, 0xF7, 0x02, 0x00, 0x00, 0x01});
    CHECK(store.assembled == store.original);
}

TEST_CASE("an absolute operand that has no base falls back to rip-relative", "[disasm]")
{
    // `lea` has no absolute memory form, and the decoder reports no memory
    // operand for it, so the fallback keeps its rip-relative form.
    const auto lea = round_trip({0x48, 0x8D, 0x05, 0xF7, 0x02, 0x00, 0x00});
    CHECK(lea.assembled == lea.original);
}

TEST_CASE("a block of instructions assembles in order", "[disasm]")
{
    CHECK(block("MOV RBP, RSP\nRET") == bytes({0x48, 0x89, 0xE5, 0xC3}));
    CHECK(block("NOP\nRET") == bytes({0x90, 0xC3}));
}

TEST_CASE("each block instruction is encoded for the address it occupies", "[disasm]")
{
    // The JZ sits at 0x2001, after the NOP, and names 0x2008 from 0x2003: the
    // running address, not the block base, is what its target is measured from.
    CHECK(block("NOP\nJZ 0x2008\nRET") == bytes({0x90, 0x74, 0x05, 0xC3}));

    // A base-less MOV falls back to rip-relative from its own 0x2001, so its
    // displacement is 0x2010 - 0x2008 and proves the address advanced.
    CHECK(block("NOP\nMOV RBX, [0x2010]") == bytes({0x90, 0x48, 0x8B, 0x1D, 0x08, 0x00, 0x00, 0x00}));
}

TEST_CASE("a block assembles in the 32-bit mode", "[disasm]")
{
    CHECK(block("push ebp\nmov ebp, esp\nsub esp, 0x20", kAddress, MachineMode::legacy_32)
          == bytes({0x55, 0x89, 0xE5, 0x83, 0xEC, 0x20}));
}

TEST_CASE("blank lines and `;` comments are ignored", "[disasm]")
{
    CHECK(block("; heading\n\n  NOP  ; trailing\n\t\nRET") == bytes({0x90, 0xC3}));
}

TEST_CASE("a rejected block names its line, and an empty one is refused", "[disasm]")
{
    const auto rejected =
        slopkit::disasm::assemble_block("NOP\n; comment\nFROBNICATE RAX", AssembleBlockContext {.base = kAddress});
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error() == "line 3: unknown mnemonic 'frobnicate'");

    const auto empty =
        slopkit::disasm::assemble_block("  \n; only a comment\n", AssembleBlockContext {.base = kAddress});
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error() == "the text holds no instruction");
}
