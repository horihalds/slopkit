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
    struct AssembleContext
    {
        std::uint64_t              address {};
        MachineMode                mode {MachineMode::long_64};
        std::span<const MemoryRef> memory {};
    };

    [[nodiscard]] std::expected<std::vector<std::byte>, std::string> assemble(std::string_view       text,
                                                                              const AssembleContext& context);

    struct AssembleBlockContext
    {
        std::uint64_t base {};
        MachineMode   mode {MachineMode::long_64};
    };

    [[nodiscard]] std::expected<std::vector<std::byte>, std::string>
    assemble_block(std::string_view text, const AssembleBlockContext& context);

}
