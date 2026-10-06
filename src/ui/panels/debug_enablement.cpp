#include "ui/panels/debug_enablement.hpp"

namespace slopkit::ui::panels
{

    DebugEnablement debug_enablement(debug::Controller::State state,
                                     bool                     target_valid,
                                     bool                     has_selected_instruction,
                                     bool                     has_breakpoints) noexcept
    {
        const bool idle     = state == debug::Controller::State::idle;
        const bool stopped  = state == debug::Controller::State::stopped;
        const bool running  = state == debug::Controller::State::running;
        const bool starting = state == debug::Controller::State::starting;

        DebugEnablement enablement;
        enablement.start_stop        = idle ? target_valid : !starting;
        enablement.toggle_breakpoint = !idle && has_selected_instruction;
        enablement.resume            = stopped;
        enablement.interrupt         = running;
        enablement.step_into         = stopped;
        enablement.step_over         = stopped;
        enablement.step_out          = stopped;
        enablement.clear_breakpoints = has_breakpoints;
        return enablement;
    }

} // namespace slopkit::ui::panels
