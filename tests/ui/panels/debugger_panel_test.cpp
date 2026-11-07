#include <catch2/catch.hpp>

#include <QAbstractItemModel>
#include <QStringList>
#include <QTableView>

#include <cstdint>

#include "debug/controller.hpp"
#include "support/fake_debug.hpp"
#include "support/fake_process.hpp"
#include "ui/components/validating_delegate.hpp"
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

    CHECK(pane.register_model()->rowCount() == 18);
    CHECK_FALSE(pane.register_model()->editable());
    CHECK(pane.call_stack_table()->model()->rowCount() == 0);

    controller.start(42, "fake");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped;
                       }));
    CHECK(pane.register_model()->editable());

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
    backend.set_register_file(slopkit::tests::default_registers(0x1234));
    Controller controller(backend);

    DebuggerPanel pane(controller);

    const QString family = slopkit::ui::mono_font().family();
    CHECK(pane.register_table()->font().family() == family);
    CHECK(pane.call_stack_table()->font().family() == family);

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
          == QStringLiteral("0x0000000000001234"));
}

TEST_CASE("debugger pane reports why a register edit was turned down", "[ui][panels][debugger]")
{
    slopkit::test::application();
    FakeDebugBackend backend;
    Controller       controller(backend);

    DebuggerPanel pane(controller);

    // The register table edits through the validating delegate, so a refusal
    // keeps its editor open instead of dropping the typed value.
    CHECK(dynamic_cast<slopkit::ui::components::ValidatingDelegate*>(pane.register_table()->itemDelegate()) != nullptr);

    QStringList messages;
    QObject::connect(&pane,
                     &DebuggerPanel::statusChanged,
                     [&messages](const QString& message, bool)
                     {
                         messages.push_back(message);
                     });

    controller.start(42, "fake");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped
                               && controller.registers().size() == 18;
                       }));

    auto*             model = pane.register_model();
    const QModelIndex cell  = model->index(0, slopkit::ui::models::RegisterModel::value);
    CHECK(model->setData(cell, QStringLiteral("0x10"), Qt::EditRole));
    CHECK(messages.isEmpty());

    CHECK_FALSE(model->setData(cell, QStringLiteral("zzz"), Qt::EditRole));
    CHECK(messages == QStringList {QStringLiteral("Value: expected a number such as 1A or 0x1A.")});
}
