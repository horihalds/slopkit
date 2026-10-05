#pragma once

#include <cstddef>
#include <cstdint>

namespace slopkit::debug
{

    // How one step must reach the plugin. A plain single step leaves both fields
    // zero; when a software trap sits exactly at RIP the plugin is asked to
    // resume from that address, which lifts the int3 for one instruction and
    // re-inserts it.
    struct StepPlan
    {
        std::uint64_t resume_address {};
        std::size_t   resume_step_size {};

        [[nodiscard]] bool is_plain_step() const noexcept
        {
            return resume_address == 0;
        }
    };

    // `armed_trap` is the address of a software breakpoint sitting exactly at
    // `rip`, or 0 when there is none. `instruction_size` is the length of the
    // instruction at RIP, used when the trap has to be lifted.
    [[nodiscard]] StepPlan
    plan_step(std::uint64_t rip, std::uint64_t armed_trap, std::size_t instruction_size) noexcept;

} // namespace slopkit::debug
