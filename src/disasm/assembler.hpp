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

    // Assembles one Intel-syntax instruction - `"MOV RBP, RSP"`, `"JZ 0x4010"`,
    // `"MOV RAX, [0x22FE]"` - at `context.address`. On success the encoded
    // bytes; otherwise a message naming what was rejected. Operands are the form
    // the listing prints: registers by name, immediates and memory displacements
    // as numbers, branch targets and memory addresses absolute. A memory operand
    // whose counterpart in `context.memory` is rip-relative is encoded relative
    // to the instruction pointer again.
    [[nodiscard]] std::expected<std::vector<std::byte>, std::string> assemble(std::string_view       text,
                                                                              const AssembleContext& context);

} // namespace slopkit::disasm
