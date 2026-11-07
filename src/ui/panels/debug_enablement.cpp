#include "ui/panels/debug_enablement.hpp"

namespace slopkit::ui::panels
{
    DebugEnablement debug_enablement(debug::Controller::State state,
                                     bool                     target_valid,
                                     bool                     has_selected_instruction,
                                     bool                     has_breakpoints,
                                     bool                     debug_supported) noexcept
    {
        const bool idle     = state == debug::Controller::State::idle;
        const bool stopped  = state == debug::Controller::State::stopped;
        const bool running  = state == debug::Controller::State::running;
        const bool starting = state == debug::Controller::State::starting;

        DebugEnablement enablement;
        enablement.start_stop        = debug_supported && (idle ? target_valid : !starting);
        enablement.toggle_breakpoint = debug_supported && !idle && has_selected_instruction;
        enablement.resume            = debug_supported && stopped;
        enablement.interrupt         = debug_supported && running;
        enablement.step_into         = debug_supported && stopped;
        enablement.step_over         = debug_supported && stopped;
        enablement.step_out          = debug_supported && stopped;
        enablement.clear_breakpoints = debug_supported && has_breakpoints;
        return enablement;
    }

}
