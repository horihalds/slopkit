#pragma once

#include <cstddef>
#include <cstdint>

namespace slopkit::debug
{
    struct StepPlan
    {
        std::uint64_t resume_address {};
        std::size_t   resume_step_size {};

        [[nodiscard]] bool is_plain_step() const noexcept
        {
            return resume_address == 0;
        }
    };

    [[nodiscard]] StepPlan
    plan_step(std::uint64_t rip, std::uint64_t armed_trap, std::size_t instruction_size) noexcept;

}
