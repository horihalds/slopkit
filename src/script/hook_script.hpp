#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "disasm/decoder.hpp"

namespace slopkit::script
{
    // One decoded listing row handed to the generator: everything the hook rules
    // need about it, and nothing about the live target.
    struct HookRow
    {
        std::uint64_t                         address {};
        std::span<const std::byte>            bytes;         // the encoding, from the cache
        std::span<const disasm::AddressBytes> address_bytes; // where its printed addresses sit
        std::span<const disasm::AddressRef>   addresses;     // the printed address slices
        std::string                           text;          // as the listing prints it, addresses absolute
        bool                                  valid {};      // false for a `.byte` row
    };

    // The neighbourhood a hook may be generated from: the decoded listing rows
    // around `selected` and the module it lives in, when any.
    struct HookCandidate
    {
        std::span<const HookRow> rows;            // decoded listing rows, ascending
        std::size_t              selected {};     // index of the hooked row in `rows`
        disasm::MachineMode      mode {};         // the CPU mode the listing decoded for
        std::string              module_name;     // empty outside every module
        std::string              module_rva_text; // the RVA, upper-case hex without 0x; empty outside every module
        std::string              description;     // "game.exe+1A2B40", or the absolute address outside a module
    };

    // One instruction of the window a hook overwrites.
    struct HookInstruction
    {
        std::uint64_t             address {}; // where it sat when it was read
        std::vector<std::byte>    bytes;
        std::string               text;         // as the listing prints it
        std::vector<std::string>  rewritten;    // operand-rewritten text, or the same text
        std::vector<std::int64_t> address_args; // site-relative value per `%X`, in placeholder order
    };

    // Everything a hook script for one row is generated from.
    struct HookTarget
    {
        std::uint64_t                address {};  // the hooked instruction
        std::string                  description; // "game.exe+1A2B40", or the absolute address outside a module
        std::string                  module_name; // empty outside every module
        std::string                  module_rva_text;
        std::vector<std::byte>       original;          // the window's bytes, ascending
        std::vector<HookInstruction> covered;           // the window's instructions, in order
        std::string                  pattern;           // "48 89 E5 48 8B ?? ?? ..."
        std::size_t                  pattern_offset {}; // the window's offset inside the match
        std::size_t                  instruction_length {};
        std::vector<std::string>     trampoline_lines; // the replaced instructions, operand-rewritten
    };

    // Turns a listing neighbourhood into a hook target, or leaves it empty and
    // sets `reason`: the selected row is not a decoded instruction, the window
    // runs past the rows the listing has decoded, or the replaced instructions
    // do not re-encode.
    [[nodiscard]] std::optional<HookTarget> build_hook(const HookCandidate& candidate, std::string& reason);

    // The whole prefilled Lua source for `target`; the caller never edits it.
    [[nodiscard]] std::string render(const HookTarget& target);

    // "??": a byte that is wildcarded, "48": a literal. The wildcard offsets are
    // relative to `bytes`.
    [[nodiscard]] std::string pattern_text(std::span<const std::byte>            bytes,
                                           std::span<const disasm::AddressBytes> wildcards);
} // namespace slopkit::script
