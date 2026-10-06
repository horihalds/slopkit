#include <catch2/catch.hpp>

#include <QAbstractItemModel>
#include <QTableView>

#include <cstdint>

#include "debug/controller.hpp"
#include "support/fake_debug.hpp"
#include "support/fake_process.hpp"
#include "ui/fonts.hpp"
#include "ui/models/register_model.hpp"
#include "ui/panels/debugger_panel.hpp"

using slopkit::debug::Controller;
using slopkit::tests::FakeDebugBackend;
using slopkit::tests::pump_until;
using slopkit::ui::panels::DebuggerPanel;

TEST_CASE("debugger pane shows the register file and the call stack", "[ui][panels][debugger]")
{
    slopkit::test::application();
    FakeDebugBackend backend;
    Controller       controller(backend);

    DebuggerPanel pane(controller);

    // Idle: the placeholder register rows are present and read-only, and the
    // call stack is empty.
    CHECK(pane.register_model()->rowCount() == 18);
    CHECK_FALSE(pane.register_model()->editable());
    CHECK(pane.call_stack_table()->model()->rowCount() == 0);

    // A stopped session makes the register table editable.
    controller.start(42, "fake");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped;
                       }));
    CHECK(pane.register_model()->editable());

    // Ending the session returns it to read-only.
    controller.stop();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::idle;
                       }));
    CHECK_FALSE(pane.register_model()->editable());
}

TEST_CASE("debugger pane renders its tables in the embedded mono font", "[ui][panels][debugger]")
{
    slopkit::test::application();
    FakeDebugBackend backend;
    backend.register_file = slopkit::tests::default_registers(0x1234);
    Controller controller(backend);

    DebuggerPanel pane(controller);

    const QString family = slopkit::ui::mono_font().family();
    CHECK(pane.register_table()->font().family() == family);
    CHECK(pane.call_stack_table()->font().family() == family);

    // The registers keep the padded hex form the pane renders.
    controller.start(42, "fake");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped
                               && controller.registers().size() == 18;
                       }));
    CHECK(pane.register_model()
              ->data(pane.register_model()->index(16, slopkit::ui::models::RegisterModel::value), Qt::DisplayRole)
              .toString()
          == QStringLiteral("0000000000001234"));
}
