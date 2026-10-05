#include "debug/step_over.hpp"

namespace slopkit::debug
{

    StepPlan plan_step(std::uint64_t rip, std::uint64_t armed_trap, std::size_t instruction_size) noexcept
    {
        if (armed_trap != 0 && armed_trap == rip)
        {
            return StepPlan {rip, instruction_size != 0 ? instruction_size : 1};
        }
        return StepPlan {};
    }

} // namespace slopkit::debug
