#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "disasm/decoder.hpp"

namespace slopkit::disasm
{

    // What assembling one instruction needs besides its text: the address it is
    // encoded for - branch/call targets and memory displacements are absolute
    // there - and, when an existing instruction is being edited, its memory
    // operands, so an operand that was rip-relative stays rip-relative instead
    // of growing into a longer absolute form.
    struct AssembleContext
    {
        std::uint64_t              address {};
        MachineMode                mode {MachineMode::long_64};
        std::span<const MemoryRef> memory {}; // the edited instruction's operands; may be empty
    };

    // Assembles one Intel-syntax instruction - `"MOV RBP, RSP"`, `"JZ 4010"`,
    // `"MOV RAX, [22FE]"` - at `context.address`. On success the encoded
    // bytes; otherwise a message naming what was rejected. Operands are the form
    // the listing prints: registers by name, immediates and memory displacements
    // as numbers, branch targets and memory addresses absolute. A memory operand
    // whose counterpart in `context.memory` is rip-relative is encoded relative
    // to the instruction pointer again.
    [[nodiscard]] std::expected<std::vector<std::byte>, std::string> assemble(std::string_view       text,
                                                                              const AssembleContext& context);

    // What assembling a block of instructions needs: the address the block starts
    // at and the CPU mode every instruction is encoded for.
    struct AssembleBlockContext
    {
        std::uint64_t base {};
        MachineMode   mode {MachineMode::long_64};
    };

    // Assembles the newline-separated `text` at successive addresses from
    // `context.base`: each instruction is encoded for the address of its own first
    // byte, so a branch target or a base-less memory operand resolves against where
    // the code will actually live. Blank lines, surrounding whitespace and `;`
    // comments are ignored. On success the block's bytes in order; otherwise a
    // message naming the 1-based line and what was rejected.
    [[nodiscard]] std::expected<std::vector<std::byte>, std::string>
    assemble_block(std::string_view text, const AssembleBlockContext& context);

} // namespace slopkit::disasm
