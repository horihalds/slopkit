#include <catch2/catch.hpp>

#include <cstdint>

#include "debug/controller.hpp"
#include "support/fake_debug.hpp"
#include "ui/models/breakpoint_model.hpp"

using slopkit::debug::Controller;
using slopkit::debug::Kind;
using slopkit::tests::FakeDebugBackend;
using slopkit::ui::models::BreakpointModel;

TEST_CASE("breakpoint model mirrors the controller table", "[ui][models][breakpoint]")
{
    FakeDebugBackend backend;
    Controller       controller(backend);
    BreakpointModel  model(controller);

    CHECK(model.rowCount() == 0);
    CHECK(model.columnCount() == 5);

    const auto first  = controller.add_breakpoint("0x1000", Kind::software, 1);
    const auto second = controller.add_breakpoint("0x2000", Kind::hardware_write, 4);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    model.refresh();

    REQUIRE(model.rowCount() == 2);
    CHECK(model.data(model.index(0, BreakpointModel::kind), Qt::DisplayRole).toString() == QStringLiteral("Software"));
    CHECK(model.data(model.index(1, BreakpointModel::kind), Qt::DisplayRole).toString()
          == QStringLiteral("Hardware write"));
    CHECK(model.data(model.index(0, BreakpointModel::address), Qt::DisplayRole).toString() == QStringLiteral("0x1000"));
    CHECK(model.data(model.index(1, BreakpointModel::size), Qt::DisplayRole).toString() == QStringLiteral("4"));
    CHECK(model.data(model.index(0, BreakpointModel::enabled), Qt::CheckStateRole).toInt() == Qt::Checked);
    CHECK(model.id_at(0) == *first);
    CHECK(model.id_at(1) == *second);

    // Toggling the checkbox disables the breakpoint through the controller.
    CHECK(model.setData(model.index(0, BreakpointModel::enabled), Qt::Unchecked, Qt::CheckStateRole));
    REQUIRE(controller.table().find(*first) != nullptr);
    CHECK_FALSE(controller.table().find(*first)->enabled);
    CHECK(model.data(model.index(0, BreakpointModel::enabled), Qt::CheckStateRole).toInt() == Qt::Unchecked);

    // A hit counter is reflected after a refresh.
    controller.table().count_hit(*second);
    model.refresh();
    CHECK(model.data(model.index(1, BreakpointModel::hits), Qt::DisplayRole).toString() == QStringLiteral("1"));

    CHECK(model.headerData(BreakpointModel::enabled, Qt::Horizontal, Qt::DisplayRole).toString()
          == QStringLiteral("Enabled"));
}

TEST_CASE("breakpoint model hides the access watch entry", "[ui][models][breakpoint]")
{
    FakeDebugBackend backend;
    Controller       controller(backend);
    controller.start(42, "fake");
    REQUIRE(slopkit::tests::pump_until(controller,
                                       [&]
                                       {
                                           return controller.state() == Controller::State::stopped;
                                       }));

    const auto user = controller.add_breakpoint("0x1000", Kind::software, 1);
    REQUIRE(user.has_value());
    REQUIRE(controller.watch_address(0x4000, Kind::hardware_write, 4).has_value());

    // The watch's hidden slot is not a row here, and Clear All still removes it.
    BreakpointModel model(controller);
    REQUIRE(model.rowCount() == 1);
    CHECK(model.id_at(0) == *user);

    controller.clear_breakpoints();
    model.refresh();
    CHECK(model.rowCount() == 0);
    CHECK(controller.breakpoints().empty());
}
