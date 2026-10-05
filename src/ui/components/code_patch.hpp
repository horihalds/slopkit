#pragma once

#include <cstddef>
#include <cstdint>
#include <flat_map>
#include <vector>

#include "process/types.hpp"

#include <QString>

namespace slopkit::ui::components
{

    // One instruction this session replaced in the attached target - with NOP
    // bytes, or with an instruction the user assembled.
    struct CodePatch
    {
        std::uint64_t          begin {};       // the replaced instruction's first byte
        std::size_t            length {};      // its whole length, in bytes
        std::vector<std::byte> original_bytes; // what the target held before the patch
        QString                original_text;  // the instruction text it held
        bool                   nop {};         // the replacement was NOP bytes, not an edited instruction

        [[nodiscard]] std::uint64_t end() const noexcept
        {
            return begin + length;
        }
    };

    // The code patches of one Memory Viewer session, keyed to the pid they were
    // applied to. It is bookkeeping for the disassembly pane: it owns no widget
    // and never touches the target.
    class CodePatchTable
    {
    public:
        // Records the NOP applied to the instruction at `begin`; replaces an
        // earlier record of the same address.
        const CodePatch&
        apply_nop(std::uint64_t begin, std::size_t length, std::vector<std::byte> original, QString original_text);
        // Records the assembled replacement of the instruction at `begin`; the
        // record keeps the original so Restore can put it back.
        const CodePatch&
        apply_edit(std::uint64_t begin, std::size_t length, std::vector<std::byte> original, QString original_text);
        // Forgets the record of the instruction at `begin`; false when none is.
        bool                           restore(std::uint64_t begin);
        // The record whose region covers `address`, so a click anywhere in a NOP
        // run reaches the instruction it replaced; null when none does.
        [[nodiscard]] const CodePatch* covering(std::uint64_t address) const;
        // Drops every record when the attached target changed (0 when detached).
        void                           note_target(process::ProcessId pid);
        [[nodiscard]] std::size_t      size() const noexcept;
        [[nodiscard]] bool             empty() const noexcept;

    private:
        std::flat_map<std::uint64_t, CodePatch> patches_; // keyed by begin, in address order
        process::ProcessId                      pid_ {0};
    };

    // The bytes an inline NOP patch writes: one 0x90 per byte of the instruction.
    [[nodiscard]] std::vector<std::byte> nop_bytes(std::size_t length);

} // namespace slopkit::ui::components
