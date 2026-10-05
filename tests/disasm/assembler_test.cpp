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

TEST_CASE("branch targets are assembled as absolute addresses", "[disasm]")
{
    CHECK(encoded("JZ 0x2017") == bytes({0x74, 0x15}));
    CHECK(encoded("CALL 0x2017") == bytes({0xE8, 0x12, 0x00, 0x00, 0x00}));
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
    for (const auto& candidate :
         {round_trip({0x55}), round_trip({0x48, 0x89, 0xE5}), round_trip({0xC3}), round_trip({0x90})})
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
