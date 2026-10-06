#pragma once

#include "debug/controller.hpp"

namespace slopkit::ui::panels
{
    // What the debugger's commands are allowed to do in the current session
    // state. `DebugControls` and `ViewerMenu` both render this, so the one-line
    // control bar and the Debug menu can never disagree.
    struct DebugEnablement
    {
        bool start_stop {};        // idle: needs a valid target; starting: no
        bool toggle_breakpoint {}; // a session plus a selected instruction
        bool resume {};            // stopped
        bool interrupt {};         // running
        bool step_into {};         // stopped
        bool step_over {};         // stopped
        bool clear_breakpoints {}; // at least one breakpoint armed
    };

    [[nodiscard]] DebugEnablement debug_enablement(debug::Controller::State state,
                                                   bool                     target_valid,
                                                   bool                     has_selected_instruction,
                                                   bool                     has_breakpoints) noexcept;
} // namespace slopkit::ui::panels
