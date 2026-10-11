#pragma once

#include "debug/controller.hpp"

namespace slopkit::ui::panels
{
    struct DebugEnablement
    {
        bool start_stop {};
        bool toggle_breakpoint {};
        bool resume {};
        bool interrupt {};
        bool step_into {};
        bool step_over {};
        bool step_out {};
        bool clear_breakpoints {};
    };

    [[nodiscard]] DebugEnablement debug_enablement(debug::Controller::State state,
                                                   bool                     target_valid,
                                                   bool                     has_selected_instruction,
                                                   bool                     has_breakpoints) noexcept;
}
