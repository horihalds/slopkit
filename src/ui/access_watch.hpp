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
    struct ResolvedAccess
    {
        std::uint64_t address {};
        QString       operand;
        std::size_t   width {1};
        bool          writes {};
        bool          resolved {true};
    };

    [[nodiscard]] std::vector<ResolvedAccess> resolve_accesses(std::span<const disasm::MemoryRef> operands,
                                                               std::uint64_t                      instruction_address,
                                                               std::size_t                        instruction_length,
                                                               std::span<const debug::RegisterValue> registers);

    [[nodiscard]] std::size_t watch_size(std::size_t width) noexcept;

}
