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

    // One address an instruction's text prints - a branch/call target or a
    // memory operand - as a slice of `Instruction::text` plus the address it
    // names. The decoder stays Qt-free, so a listing renders the slice in
    // whatever form it shows other addresses.
    struct AddressRef
    {
        std::size_t   offset {};  // byte offset into Instruction::text (the text is ASCII)
        std::size_t   length {};  // length of the address text inside that slice
        std::uint64_t address {}; // what the printed text names
    };

    // The memory one explicit operand touches, expressed so a caller that knows
    // the register file can compute the address. A sibling of AddressRef, which
    // only carries what the printed text names: the decoder stays ignorant of
    // any register context, and the resolver supplies it.
    struct MemoryRef
    {
        std::size_t  offset {}; // slice of Instruction::text, as AddressRef uses
        std::size_t  length {};
        std::string  base;  // register name, empty when the operand has none
        std::string  index; // register name, empty when the operand has none
        std::uint8_t scale {1};
        std::int64_t displacement {};
        std::uint8_t width {};        // bytes the operand accesses
        bool         rip_relative {}; // the displacement is measured from the next instruction
        bool         writes {};       // the operand is a destination
    };

    // What a slice of Instruction::text is, for a listing that paints each kind
    // in a colour of its own. The mnemonic, the brackets and the commas are not
    // classified and stay in the default colour.
    enum class TokenKind
    {
        cpu_register, // a register literal (`RAX`)
        immediate,    // an immediate literal (`1`)
        displacement, // a memory displacement or relative address (`-04`)
    };

    // One classified slice of text, as AddressRef slices it.
    struct TokenSpan
    {
        std::size_t offset {}; // byte offset into Instruction::text (the text is ASCII)
        std::size_t length {};
        TokenKind   kind {};
    };

    // One decoded instruction. `valid` is false for a byte Zydis rejects, which
    // is rendered as a one-byte `.byte NN` row so a listing never stalls.
    struct Instruction
    {
        std::uint64_t           address {};
        std::size_t             length {};
        std::string             text;
        bool                    valid {};
        std::vector<AddressRef> addresses; // in the order the text prints them; empty for `.byte`
        std::vector<MemoryRef>  memory;    // explicit operands only; empty for `lea` and `.byte`
        std::vector<TokenSpan>  tokens;    // in printed order; empty for `RET` and `.byte`
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
