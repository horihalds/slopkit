#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace slopkit::disasm
{

    // The CPU mode instructions are decoded in. The native target is 64-bit, so
    // `long_64` is the default everywhere; `legacy_32` exists for the 32-bit
    // images a plugin may eventually report.
    enum class MachineMode
    {
        long_64,
        legacy_32,
    };

    // One decoded instruction. `valid` is false for a byte Zydis rejects, which
    // is rendered as a one-byte `.byte 0xNN` row so a listing never stalls.
    struct Instruction
    {
        std::uint64_t address {};
        std::size_t   length {};
        std::string   text;
        bool          valid {};
    };

    // Decodes the instruction at `address` from the front of `code`. Returns
    // `std::nullopt` for an empty buffer; a byte Zydis rejects comes back as an
    // invalid one-byte row.
    [[nodiscard]] std::optional<Instruction>
    decode(std::span<const std::byte> code, std::uint64_t address, MachineMode mode = MachineMode::long_64);

    // Decodes up to `count` consecutive instructions starting at `base`. The
    // sweep stops at the end of the buffer and when the remaining bytes are not
    // enough for a complete instruction.
    [[nodiscard]] std::vector<Instruction> decode_block(std::span<const std::byte> code,
                                                        std::uint64_t              base,
                                                        std::size_t                count,
                                                        MachineMode                mode = MachineMode::long_64);

} // namespace slopkit::disasm
