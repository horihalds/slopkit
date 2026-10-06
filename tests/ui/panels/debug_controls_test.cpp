#include <catch2/catch.hpp>

#include <QPushButton>

#include <cstdint>

#include "debug/controller.hpp"
#include "process/attachment.hpp"
#include "support/fake_debug.hpp"
#include "support/fake_process.hpp"
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

    // Idle with an attached target: the toggle reads Start and is live, while
    // the run controls wait for a session.
    CHECK(bar.start_stop_button()->text() == QStringLiteral("Start Debugging"));
    CHECK(bar.start_stop_button()->isEnabled());
    CHECK_FALSE(bar.resume_button()->isEnabled());
    CHECK_FALSE(bar.break_button()->isEnabled());
    CHECK_FALSE(bar.step_into_button()->isEnabled());
    CHECK_FALSE(bar.step_over_button()->isEnabled());
    CHECK(bar.breakpoints_button()->isEnabled());

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

    // One click starts the session, attaching and leaving the target running:
    // the toggle flips to Stop and the run controls to the running set.
    backend.block_continue = true;
    bar.start_stop_button()->click();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::running;
                       }));

    CHECK(bar.start_stop_button()->text() == QStringLiteral("Stop Debugging"));
    CHECK(bar.start_stop_button()->isEnabled());
    CHECK_FALSE(bar.resume_button()->isEnabled());
    CHECK_FALSE(bar.step_into_button()->isEnabled());
    CHECK_FALSE(bar.step_over_button()->isEnabled());
    CHECK(bar.break_button()->isEnabled());

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
    CHECK(bar.start_stop_button()->text() == QStringLiteral("Stop Debugging"));

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

    // The next click on the toggle stops the session and returns the bar to
    // idle.
    bar.start_stop_button()->click();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::idle;
                       }));
    CHECK(bar.start_stop_button()->text() == QStringLiteral("Start Debugging"));
    CHECK(bar.start_stop_button()->isEnabled());
}

TEST_CASE("the debug control bar's disabled toggle attaches nothing", "[ui][panels][debugger]")
{
    slopkit::test::application();
    FakeDebugBackend backend;
    Controller       controller(backend);

    const slopkit::process::AttachedTarget detached;
    DebugControls                          bar(controller, detached);

    // No target: the toggle reads Start and is disabled, so clicking it attaches
    // nothing.
    CHECK(bar.start_stop_button()->text() == QStringLiteral("Start Debugging"));
    CHECK_FALSE(bar.start_stop_button()->isEnabled());
    bar.start_stop_button()->click();
    CHECK(controller.state() == Controller::State::idle);
    CHECK(backend.count("attach") == 0);
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
