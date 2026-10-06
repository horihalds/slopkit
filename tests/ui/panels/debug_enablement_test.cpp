#include <catch2/catch.hpp>

#include "ui/panels/debug_enablement.hpp"

using slopkit::debug::Controller;
using slopkit::ui::panels::debug_enablement;
using slopkit::ui::panels::DebugEnablement;

namespace
{
    constexpr Controller::State states[] = {
        Controller::State::idle,
        Controller::State::starting,
        Controller::State::stopped,
        Controller::State::running,
    };
} // namespace

TEST_CASE("the shared debugger enablement covers every session state", "[ui][panels][debugger]")
{
    // The full truth table: session state x target validity x selection x
    // breakpoints. Step Into, Step Over and Step Out share the one stopped gate.
    for (const Controller::State state : states)
    {
        for (const bool target_valid : {false, true})
        {
            for (const bool has_selection : {false, true})
            {
                for (const bool has_breakpoints : {false, true})
                {
                    INFO("state=" << static_cast<int>(state) << " target_valid=" << target_valid
                                  << " has_selection=" << has_selection << " has_breakpoints=" << has_breakpoints);
                    const DebugEnablement enablement =
                        debug_enablement(state, target_valid, has_selection, has_breakpoints);

                    const bool idle     = state == Controller::State::idle;
                    const bool starting = state == Controller::State::starting;
                    const bool stopped  = state == Controller::State::stopped;
                    const bool running  = state == Controller::State::running;

                    CHECK(enablement.start_stop == (idle ? target_valid : !starting));
                    CHECK(enablement.toggle_breakpoint == (!idle && has_selection));
                    CHECK(enablement.resume == stopped);
                    CHECK(enablement.interrupt == running);
                    CHECK(enablement.step_into == stopped);
                    CHECK(enablement.step_over == stopped);
                    CHECK(enablement.step_out == stopped);
                    CHECK(enablement.clear_breakpoints == has_breakpoints);
                }
            }
        }
    }
}
