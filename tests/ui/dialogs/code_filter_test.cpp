#include <catch2/catch.hpp>

#include <cstdint>
#include <vector>

#include "support/ui_helpers.hpp"
#include "ui/dialogs/code_filter.hpp"

using slopkit::debug::CodeFilterCandidate;
using slopkit::debug::Controller;
using slopkit::ui::dialogs::CodeFilterDialog;

namespace
{
    using slopkit::tests::pump_until;

    slopkit::process::ModuleInfo game_module()
    {
        slopkit::process::ModuleInfo module;
        module.name         = "game.so";
        module.base         = 0x1000;
        module.size         = 0x2000;
        module.pointer_size = 8;
        return module;
    }
}

TEST_CASE("the code filter window collects with or without a running session", "[ui]")
{
    application();

    slopkit::tests::FakeDebugBackend backend;
    Controller                       controller {backend};
    backend.block_continue = true;
    controller.start(42, "fake");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::running;
                       }));

    slopkit::process::AttachedTarget target = fake_target();
    CodeFilterDialog                 dialog {controller, target};

    CHECK_FALSE(dialog.collect_button()->isEnabled());
    CHECK_FALSE(dialog.hint_text().contains(QStringLiteral("Pause")));
    CHECK(dialog.hint_text() == QStringLiteral("Attach to a target to collect."));

    dialog.set_modules({game_module()});
    CHECK(dialog.collect_button()->isEnabled());
    CHECK_FALSE(dialog.hint_text().contains(QStringLiteral("Pause")));
    CHECK(dialog.hint_text() == QStringLiteral("Collect while the target runs; hits mark themselves executed."));

    controller.start_code_filter(std::vector<CodeFilterCandidate> {CodeFilterCandidate {.target = 0x1000}});
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.code_filter().armed_count() == 1;
                       }));
    dialog.refresh();
    CHECK(dialog.collect_button()->isEnabled());
    CHECK_FALSE(dialog.hint_text().contains(QStringLiteral("Pause")));
    CHECK(
        dialog.hint_text()
        == QStringLiteral("Filter the list with the Keep buttons; the filter keeps collecting while the target runs."));
}

TEST_CASE("the code filter window disables collect with no modules", "[ui]")
{
    application();

    slopkit::tests::FakeDebugBackend backend;
    Controller                       controller {backend};
    controller.start(42, "fake");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped;
                       }));

    slopkit::process::AttachedTarget target = fake_target();
    CodeFilterDialog                 dialog {controller, target};

    CHECK_FALSE(dialog.collect_button()->isEnabled());
    CHECK(dialog.hint_text() == QStringLiteral("Attach to a target to collect."));
    CHECK_FALSE(dialog.hint_text().contains(QStringLiteral("Pause")));
}
