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
    struct HookRow
    {
        std::uint64_t                          address {};
        std::span<const std::byte>             bytes;
        std::span<const disasm::EncodingBytes> pattern_bytes;
        std::span<const disasm::AddressRef>    addresses;
        std::span<const disasm::MemoryRef>     memory;
        std::string                            text;
        bool                                   valid {};
    };

    struct HookCandidate
    {
        std::span<const HookRow> rows;
        std::size_t              selected {};
        disasm::MachineMode      mode {};
        std::string              module_name;
        std::string              module_rva_text;
        std::string              description;
    };

    struct HookInstruction
    {
        std::uint64_t             address {};
        std::vector<std::byte>    bytes;
        std::string               text;
        std::vector<std::string>  rewritten;
        std::vector<std::int64_t> address_args;
    };

    struct HookTarget
    {
        std::uint64_t                address {};
        std::string                  description;
        std::string                  module_name;
        std::string                  module_rva_text;
        std::vector<std::byte>       original;
        std::vector<HookInstruction> covered;
        std::string                  pattern;
        std::size_t                  pattern_offset {};
        std::size_t                  instruction_length {};
        std::vector<std::string>     trampoline_lines;
    };

    [[nodiscard]] std::optional<HookTarget> build_hook(const HookCandidate& candidate, std::string& reason);

    [[nodiscard]] std::string render(const HookTarget& target);

    [[nodiscard]] std::string pattern_text(std::span<const std::byte>             bytes,
                                           std::span<const disasm::EncodingBytes> wildcards);
}
