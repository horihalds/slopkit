#include <catch2/catch.hpp>

#include <QPushButton>

#include <cstdint>

#include "debug/controller.hpp"
#include "process/attachment.hpp"
#include "support/fake_debug.hpp"
#include "support/fake_process.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"
#include "ui/panels/debug_controls.hpp"

using slopkit::debug::Controller;
using slopkit::tests::FakeDebugBackend;
using slopkit::tests::pump_until;
using slopkit::ui::panels::DebugControls;

namespace
{
    slopkit::process::AttachedTarget attached_target()
    {
        slopkit::process::AttachedTarget target;
        target.pid          = 4242;
        target.plugin_id    = "linux-proc";
        target.session_live = true;
        return target;
    }
} // namespace

TEST_CASE("the debug control bar gates the controls on the session state", "[ui][panels][debugger]")
{
    slopkit::test::application();
    FakeDebugBackend backend;
    Controller       controller(backend);

    const slopkit::process::AttachedTarget target = attached_target();
    DebugControls                          bar(controller, target);

    // Idle with an attached target: only Start and Breakpoints are live.
    CHECK(bar.start_button()->isEnabled());
    CHECK_FALSE(bar.stop_button()->isEnabled());
    CHECK_FALSE(bar.resume_button()->isEnabled());
    CHECK_FALSE(bar.break_button()->isEnabled());
    CHECK_FALSE(bar.step_into_button()->isEnabled());
    CHECK_FALSE(bar.step_over_button()->isEnabled());
    CHECK(bar.breakpoints_button()->isEnabled());
    CHECK(bar.status_label()->text() == QStringLiteral("Not debugging"));

    // The Breakpoints button only asks the window to open the dialog.
    int requests = 0;
    QObject::connect(&bar,
                     &DebugControls::breakpointsRequested,
                     &bar,
                     [&requests]
                     {
                         ++requests;
                     });
    bar.breakpoints_button()->click();
    CHECK(requests == 1);

    // Starting the session attaches and leaves the target running: the run
    // controls flip to the running set.
    backend.block_continue = true;
    controller.start(target.pid, target.plugin_id);
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::running;
                       }));

    CHECK_FALSE(bar.start_button()->isEnabled());
    CHECK(bar.stop_button()->isEnabled());
    CHECK_FALSE(bar.resume_button()->isEnabled());
    CHECK_FALSE(bar.step_into_button()->isEnabled());
    CHECK_FALSE(bar.step_over_button()->isEnabled());
    CHECK(bar.break_button()->isEnabled());
    CHECK(bar.status_label()->text() == QStringLiteral("Running..."));

    // Break stops the target deliberately and re-enables the step controls.
    controller.interrupt();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped;
                       }));
    CHECK(bar.resume_button()->isEnabled());
    CHECK(bar.step_into_button()->isEnabled());
    CHECK(bar.step_over_button()->isEnabled());
    CHECK_FALSE(bar.break_button()->isEnabled());
    CHECK(bar.status_label()->text().startsWith(QStringLiteral("Stopped at")));

    // Resume blocks in the fake until Break releases it; the bar follows.
    backend.continue_released = false;
    controller.resume();
    CHECK_FALSE(bar.resume_button()->isEnabled());
    CHECK(bar.break_button()->isEnabled());

    controller.interrupt();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped;
                       }));
    CHECK(bar.resume_button()->isEnabled());
    CHECK_FALSE(bar.break_button()->isEnabled());

    // Stopping the session returns the bar to idle.
    controller.stop();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::idle;
                       }));
    CHECK(bar.start_button()->isEnabled());
    CHECK_FALSE(bar.stop_button()->isEnabled());
}

TEST_CASE("the debug control bar explains the missing target", "[ui][panels][debugger]")
{
    slopkit::test::application();
    FakeDebugBackend backend;
    Controller       controller(backend);

    const slopkit::process::AttachedTarget detached;
    DebugControls                          bar(controller, detached);

    // No target: Start is disabled and clicking it performs no attach.
    CHECK_FALSE(bar.start_button()->isEnabled());
    bar.start_button()->click();
    CHECK(controller.state() == Controller::State::idle);
}

TEST_CASE("the debug control bar keeps Step Out disabled", "[ui][panels][debugger]")
{
    slopkit::test::application();
    FakeDebugBackend backend;
    backend.block_continue = true;
    Controller controller(backend);

    const slopkit::process::AttachedTarget target = attached_target();
    DebugControls                          bar(controller, target);
    CHECK_FALSE(bar.step_out_button()->isEnabled());
    CHECK(bar.step_out_button()->toolTip() == QStringLiteral("Stepping out is not supported yet."));

    // A session, running and stopped, never lights it up.
    controller.start(target.pid, target.plugin_id);
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::running;
                       }));
    CHECK_FALSE(bar.step_out_button()->isEnabled());

    controller.interrupt();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped;
                       }));
    CHECK_FALSE(bar.step_out_button()->isEnabled());
}

TEST_CASE("the debug control bar gates Toggle Breakpoint on a session and a selection", "[ui][panels][debugger]")
{
    slopkit::test::application();
    FakeDebugBackend backend;
    backend.block_continue = true;
    Controller controller(backend);

    const slopkit::process::AttachedTarget target = attached_target();
    DebugControls                          bar(controller, target);

    // No session: the toggle is off even with an instruction selected.
    bar.set_selected_instruction(0x2000, QStringLiteral("0000000000002000"));
    CHECK_FALSE(bar.toggle_breakpoint_button()->isEnabled());
    CHECK(bar.toggle_breakpoint_button()->toolTip() == QStringLiteral("Start the debugger to set breakpoints."));

    // A session but no selection: still off, and the tooltip asks for one.
    controller.start(target.pid, target.plugin_id);
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::running;
                       }));
    bar.set_selected_instruction(std::nullopt, QString());
    CHECK_FALSE(bar.toggle_breakpoint_button()->isEnabled());
    CHECK(bar.toggle_breakpoint_button()->toolTip() == QStringLiteral("Select an instruction in the listing first."));

    // Session plus a selected instruction: live, and the tooltip names the
    // address and the action.
    bar.set_selected_instruction(0x2000, QStringLiteral("0000000000002000"));
    CHECK(bar.toggle_breakpoint_button()->isEnabled());
    CHECK(bar.toggle_breakpoint_button()->toolTip() == QStringLiteral("Set a breakpoint at 0000000000002000."));

    std::uint64_t requested_address = 0;
    QString       requested_text;
    QObject::connect(&bar,
                     &DebugControls::toggleBreakpointRequested,
                     &bar,
                     [&](std::uint64_t address, const QString& expression)
                     {
                         requested_address = address;
                         requested_text    = expression;
                     });
    bar.toggle_breakpoint_button()->click();
    CHECK(requested_address == 0x2000);
    CHECK(requested_text == QStringLiteral("0000000000002000"));

    // With an entry at that address the tooltip offers removal.
    const auto added = controller.table().add(
        QStringLiteral("0000000000002000").toStdString(), 0x2000, slopkit::debug::Kind::software, 1);
    REQUIRE(added.has_value());
    bar.set_selected_instruction(0x2000, QStringLiteral("0000000000002000"));
    CHECK(bar.toggle_breakpoint_button()->toolTip() == QStringLiteral("Remove the breakpoint at 0000000000002000."));

    // Ending the session disables the toggle even while the instruction stays
    // selected.
    controller.stop();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::idle;
                       }));
    bar.set_selected_instruction(0x2000, QStringLiteral("0000000000002000"));
    CHECK_FALSE(bar.toggle_breakpoint_button()->isEnabled());
}

TEST_CASE("the debug control bar renders the state line in the embedded mono font", "[ui][panels][debugger]")
{
    slopkit::test::application();
    FakeDebugBackend backend;
    Controller       controller(backend);

    DebugControls bar(controller, attached_target());
    CHECK(bar.status_label()->font().family() == slopkit::ui::mono_font().family());
}
