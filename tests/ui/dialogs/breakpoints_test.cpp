#include <catch2/catch.hpp>

#include <cstdint>

#include <QPushButton>
#include <QTableView>

#include "debug/controller.hpp"
#include "support/fake_debug.hpp"
#include "support/fake_process.hpp"
#include "ui/components/widgets.hpp"
#include "ui/dialogs/breakpoints.hpp"
#include "ui/models/breakpoint_model.hpp"

using slopkit::debug::Controller;
using slopkit::debug::Kind;
using slopkit::tests::FakeDebugBackend;
using slopkit::ui::dialogs::BreakpointsDialog;

TEST_CASE("breakpoints window adds, removes and clears breakpoints", "[ui][dialogs][breakpoints]")
{
    slopkit::test::application();
    FakeDebugBackend backend;
    Controller       controller(backend);

    BreakpointsDialog dialog(controller);
    CHECK(dialog.model()->rowCount() == 0);
    CHECK(dialog.hint_label() != nullptr);

    // Adds by absolute expression with the default software kind.
    CHECK(dialog.add_breakpoint_from_text(QStringLiteral("0x1000")).isEmpty());
    CHECK(dialog.model()->rowCount() == 1);
    CHECK(controller.breakpoints().size() == 1);
    CHECK(dialog.table()->model()->rowCount() == 1);

    // An unresolvable expression is reported and adds nothing.
    const QString reason = dialog.add_breakpoint_from_text(QStringLiteral("no-such-module+10"));
    CHECK_FALSE(reason.isEmpty());
    CHECK(dialog.model()->rowCount() == 1);

    // A hardware kind and size are honoured.
    dialog.kind_combo()->setCurrentIndex(2); // hardware write
    dialog.size_combo()->setCurrentIndex(2); // four bytes
    CHECK(dialog.add_breakpoint_from_text(QStringLiteral("0x2000")).isEmpty());
    REQUIRE(dialog.model()->rowCount() == 2);

    const auto entries = controller.breakpoints();
    REQUIRE(entries.size() == 2);
    const slopkit::debug::Breakpoint& hardware = entries[1];
    CHECK(hardware.kind == Kind::hardware_write);
    CHECK(hardware.size == 4);

    // Removing the selected row drops it from the table and the controller.
    dialog.table()->selectRow(0);
    dialog.remove_button()->click();
    CHECK(dialog.model()->rowCount() == 1);
    CHECK(controller.breakpoints().size() == 1);

    // Clear All empties both.
    dialog.clear_button()->click();
    CHECK(dialog.model()->rowCount() == 0);
    CHECK(controller.breakpoints().empty());
}

TEST_CASE("breakpoints window mirrors an external change", "[ui][dialogs][breakpoints]")
{
    slopkit::test::application();
    FakeDebugBackend backend;
    Controller       controller(backend);

    BreakpointsDialog dialog(controller);
    REQUIRE(controller.add_breakpoint("0x1000", Kind::software, 1).has_value());

    // The dialog refreshes itself on breakpointsChanged.
    CHECK(dialog.model()->rowCount() == 1);

    controller.clear_breakpoints();
    CHECK(dialog.model()->rowCount() == 0);
}

TEST_CASE("breakpoints window keeps the access watch out", "[ui][dialogs][breakpoints]")
{
    slopkit::test::application();
    FakeDebugBackend backend;
    Controller       controller(backend);
    controller.start(42, "fake");
    REQUIRE(slopkit::tests::pump_until(controller,
                                       [&]
                                       {
                                           return controller.state() == Controller::State::stopped;
                                       }));

    BreakpointsDialog dialog(controller);
    REQUIRE(controller.add_breakpoint("0x1000", Kind::software, 1).has_value());
    CHECK(dialog.model()->rowCount() == 1);

    // The watch's hidden entry owns a slot but is not listed here.
    REQUIRE(controller.watch_address(0x4000, Kind::hardware_write, 4).has_value());
    CHECK(dialog.model()->rowCount() == 1);

    // Clear All is still everything, hidden entry included.
    dialog.clear_button()->click();
    CHECK(dialog.model()->rowCount() == 0);
    CHECK(controller.breakpoints().empty());
}
