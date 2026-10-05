#include <catch2/catch.hpp>

#include <QAbstractItemModel>
#include <QPushButton>
#include <QTableView>

#include <cstdint>

#include "debug/controller.hpp"
#include "process/attachment.hpp"
#include "support/fake_debug.hpp"
#include "support/fake_process.hpp"
#include "ui/models/register_model.hpp"
#include "ui/panels/debugger_panel.hpp"

using slopkit::debug::Controller;
using slopkit::tests::FakeDebugBackend;
using slopkit::tests::pump_until;
using slopkit::ui::panels::DebuggerPanel;

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

TEST_CASE("debugger pane gates the controls on the session state", "[ui][panels][debugger]")
{
    slopkit::test::application();
    FakeDebugBackend backend;
    Controller       controller(backend);

    const slopkit::process::AttachedTarget target = attached_target();
    DebuggerPanel                          pane(controller, target);

    // Idle with an attached target: only Start and Breakpoints are live.
    CHECK(pane.start_button()->isEnabled());
    CHECK_FALSE(pane.stop_button()->isEnabled());
    CHECK_FALSE(pane.resume_button()->isEnabled());
    CHECK_FALSE(pane.break_button()->isEnabled());
    CHECK_FALSE(pane.step_into_button()->isEnabled());
    CHECK_FALSE(pane.step_over_button()->isEnabled());
    CHECK(pane.breakpoints_button()->isEnabled());
    CHECK(pane.register_model()->rowCount() == 18);
    CHECK_FALSE(pane.register_model()->editable());
    CHECK(pane.call_stack_table()->model()->rowCount() == 0);

    // The Breakpoints button only asks the window to open the dialog.
    int requests = 0;
    QObject::connect(&pane,
                     &DebuggerPanel::breakpointsRequested,
                     &pane,
                     [&requests]
                     {
                         ++requests;
                     });
    pane.breakpoints_button()->click();
    CHECK(requests == 1);

    // Starting the session through the controller flips the pane to stopped.
    controller.start(target.pid, target.plugin_id);
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped;
                       }));

    CHECK_FALSE(pane.start_button()->isEnabled());
    CHECK(pane.stop_button()->isEnabled());
    CHECK(pane.resume_button()->isEnabled());
    CHECK(pane.step_into_button()->isEnabled());
    CHECK(pane.step_over_button()->isEnabled());
    CHECK_FALSE(pane.break_button()->isEnabled());
    CHECK(pane.register_model()->editable());

    // Resume blocks in the fake until Break releases it; the pane follows.
    backend.block_continue = true;
    controller.resume();
    CHECK_FALSE(pane.resume_button()->isEnabled());
    CHECK(pane.break_button()->isEnabled());

    controller.interrupt();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped;
                       }));
    CHECK(pane.resume_button()->isEnabled());
    CHECK_FALSE(pane.break_button()->isEnabled());

    // Stopping the session returns the pane to idle.
    controller.stop();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::idle;
                       }));
    CHECK(pane.start_button()->isEnabled());
    CHECK_FALSE(pane.stop_button()->isEnabled());
    CHECK_FALSE(pane.register_model()->editable());
}

TEST_CASE("debugger pane explains the missing target", "[ui][panels][debugger]")
{
    slopkit::test::application();
    FakeDebugBackend backend;
    Controller       controller(backend);

    const slopkit::process::AttachedTarget detached;
    DebuggerPanel                          pane(controller, detached);

    // No target: Start is disabled and a click explains why.
    CHECK_FALSE(pane.start_button()->isEnabled());
    pane.start_button()->click();
    CHECK(controller.state() == Controller::State::idle);
}
