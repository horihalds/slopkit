#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "debug/backend.hpp"
#include "disasm/decoder.hpp"

#include <QString>

namespace slopkit::ui
{

    // One memory operand with the address it resolves to in the register context
    // it was resolved against.
    struct ResolvedAccess
    {
        std::uint64_t address {};
        QString       operand; // "[RAX+RCX*4+10]", the operand as the decoder prints it
        std::size_t   width {1};
        bool          writes {};
        bool          resolved {true}; // false when a register the operand needs is unknown
    };

    // Every operand resolved against `registers`; an operand naming a register
    // that is absent stays in the result with `resolved == false`, so the caller
    // can still show it. A rip-relative displacement is measured from the end of
    // the instruction. Both a rip-relative and an absolute operand print as the
    // 16-digit address Zydis shows.
    [[nodiscard]] std::vector<ResolvedAccess> resolve_accesses(std::span<const disasm::MemoryRef> operands,
                                                               std::uint64_t                      instruction_address,
                                                               std::size_t                        instruction_length,
                                                               std::span<const debug::RegisterValue> registers);

    // Snaps a requested width to the 1/2/4/8 bytes a hardware data breakpoint can
    // watch; 0 or an unsupported width becomes 4.
    [[nodiscard]] std::size_t watch_size(std::size_t width) noexcept;

} // namespace slopkit::ui
