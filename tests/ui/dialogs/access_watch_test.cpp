#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <utility>
#include <vector>

#include "support/ui_helpers.hpp"
#include "ui/dialogs/access_watch.hpp"
#include "ui/fonts.hpp"

using slopkit::debug::Controller;
using slopkit::debug::Kind;
using slopkit::debug::WatchState;
using slopkit::ui::ResolvedAccess;
using slopkit::ui::dialogs::AccessWatchDialog;

namespace
{
    using slopkit::tests::pump_until;

    const slopkit::debug::Breakpoint* watch_entry(const Controller& controller)
    {
        for (const slopkit::debug::Breakpoint& entry : controller.breakpoints())
        {
            if (entry.hidden)
            {
                return &entry;
            }
        }
        return nullptr;
    }
} // namespace

TEST_CASE("the access watch window renders the watch and its coalesced hits", "[ui]")
{
    application();

    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};
    attach_app_session(worker);
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
    AccessWatchDialog                dialog {controller, worker, target};
    CHECK(dialog.header_text() == QStringLiteral("No watch running."));

    // Arming through the window's own seam starts a watch and clears the status.
    dialog.start_watch(0x4000, Kind::hardware_write, 4);
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           const auto* entry = watch_entry(controller);
                           return entry != nullptr && entry->armed;
                       }));
    CHECK(controller.watch().address() == 0x4000);
    CHECK(dialog.status_text().isEmpty());
    CHECK(dialog.header_text().startsWith(QStringLiteral("Watching 4000")));
    CHECK(dialog.header_text().contains(QStringLiteral("writes")));
    CHECK(dialog.header_text().contains(QStringLiteral("4 bytes")));

    // Two stops on the watched slot coalesce into one row with a count, and the
    // code is recovered by decoding the bytes ending at the reported RIP.
    const std::uint32_t slot       = watch_entry(controller)->slot;
    const std::uint64_t rip        = 0x5003;
    (*access.memory)[rip - 31]     = std::vector<std::byte>(32, std::byte {0x90});
    (*access.memory)[rip - 31][28] = std::byte {0x8B};
    (*access.memory)[rip - 31][29] = std::byte {0x43};
    (*access.memory)[rip - 31][30] = std::byte {0x10};

    backend.stop_replies.push_back(
        slopkit::debug::StopEvent {slopkit::debug::StopReason::breakpoint, 42, rip, rip, slot, 0});
    controller.interrupt();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.watch().hit_count() == 1;
                       }));
    backend.stop_replies.push_back(
        slopkit::debug::StopEvent {slopkit::debug::StopReason::breakpoint, 42, rip, rip, slot, 0});
    controller.interrupt();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.watch().hit_count() == 2;
                       }));

    REQUIRE(dialog.recorded_row_count() == 1);
    CHECK(dialog.recorded_count_at(0) == 2);
    CHECK(dialog.recorded_instruction_at(0) == rip);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return dialog.recorded_text_at(0) == QStringLiteral("MOV EAX, [RBX+10]");
                    }));

    // Follow asks for the recovered instruction address.
    std::uint64_t followed = 0;
    QObject::connect(&dialog,
                     &AccessWatchDialog::followRequested,
                     &dialog,
                     [&followed](std::uint64_t address)
                     {
                         followed = address;
                     });
    dialog.select_recorded_row(0);
    REQUIRE(dialog.follow_button()->isEnabled());
    dialog.follow_button()->click();
    CHECK(followed == rip - 3);

    // Stop leaves the rows in place; Clear empties them.
    dialog.stop_button()->click();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.watch().state() == WatchState::stopped;
                       }));
    CHECK(dialog.recorded_row_count() == 1);
    dialog.clear_button()->click();
    CHECK(dialog.recorded_row_count() == 0);
    CHECK(dialog.header_text().contains(QStringLiteral("0 accesses")));
}

TEST_CASE("the access watch window shows the resolved instruction operands", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::tests::FakeDebugBackend backend;
    Controller                       controller {backend};
    slopkit::process::AttachedTarget target = fake_target();
    AccessWatchDialog                dialog {controller, worker, target};

    std::vector<ResolvedAccess> accesses;
    ResolvedAccess              resolved;
    resolved.address  = 0x2000;
    resolved.operand  = QStringLiteral("[RAX]");
    resolved.width    = 4;
    resolved.writes   = true;
    resolved.resolved = true;
    accesses.push_back(resolved);
    ResolvedAccess unresolved;
    unresolved.operand  = QStringLiteral("[RCX]");
    unresolved.width    = 2;
    unresolved.resolved = false;
    accesses.push_back(unresolved);

    dialog.show_instruction_accesses(0x1000, 3, std::move(accesses));
    REQUIRE(dialog.instruction_row_count() == 2);
    CHECK(dialog.hint_text().contains(QStringLiteral("Resolved from the registers at 1000")));

    // A resolved row can start a watch for its address.
    dialog.select_instruction_row(0);
    REQUIRE(dialog.watch_writes_button()->isEnabled());
    REQUIRE(dialog.watch_accesses_button()->isEnabled());

    // An unresolved row cannot.
    dialog.select_instruction_row(1);
    CHECK_FALSE(dialog.watch_writes_button()->isEnabled());
    CHECK_FALSE(dialog.watch_accesses_button()->isEnabled());
}

TEST_CASE("the access watch window reports why a watch could not be armed", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::tests::FakeDebugBackend backend;
    Controller                       controller {backend};
    controller.start(42, "fake");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::stopped;
                       }));

    // Fill every debug slot, then ask the window for a watch.
    for (int index = 0; index < 4; ++index)
    {
        const auto id =
            controller.add_breakpoint(std::format("0x{:X}", 0x1000 + index * 0x100), Kind::hardware_execute, 1);
        REQUIRE(id.has_value());
    }
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           std::size_t armed = 0;
                           for (const slopkit::debug::Breakpoint& entry : controller.breakpoints())
                           {
                               armed += entry.armed ? 1 : 0;
                           }
                           return armed == 4;
                       }));

    slopkit::process::AttachedTarget target = fake_target();
    AccessWatchDialog                dialog {controller, worker, target};
    dialog.start_watch(0x9000, Kind::hardware_write, 4);
    CHECK(dialog.status_text() == QStringLiteral("no hardware slot is free"));
    CHECK(controller.watch().state() == WatchState::idle);
    CHECK(dialog.header_text() == QStringLiteral("No watch running."));
}

TEST_CASE("the access watch attaches the debugger on demand before arming", "[ui][access_watch]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::tests::FakeDebugBackend backend;
    backend.block_continue = true;
    Controller controller {backend};

    slopkit::process::AttachedTarget target = fake_target();
    AccessWatchDialog                dialog {controller, worker, target};

    int prompts = 0;
    dialog.debug_gate().set_attach_prompt(
        [&prompts](const slopkit::process::AttachedTarget&, const QString& reason)
        {
            ++prompts;
            CHECK(reason == QStringLiteral("find out what writes this address"));
            return true;
        });

    CHECK_FALSE(dialog.isVisible());
    dialog.arm_watch(0x4000, Kind::hardware_write, 4);
    CHECK(prompts == 1);
    CHECK(dialog.watch_pending());
    CHECK(controller.state() == Controller::State::starting);
    CHECK_FALSE(dialog.isVisible()); // only shown once the watch is armed

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        controller.drain();
                        const auto* entry = watch_entry(controller);
                        return entry != nullptr && entry->armed;
                    }));
    CHECK(controller.watch().address() == 0x4000);
    CHECK_FALSE(dialog.watch_pending());
    CHECK(dialog.isVisible());
    CHECK(dialog.header_text().startsWith(QStringLiteral("Watching 4000")));
}

TEST_CASE("a declined attach leaves the access watch unarmed", "[ui][access_watch]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::tests::FakeDebugBackend backend;
    Controller                       controller {backend};

    slopkit::process::AttachedTarget target = fake_target();
    AccessWatchDialog                dialog {controller, worker, target};
    dialog.debug_gate().set_attach_prompt(
        [](const slopkit::process::AttachedTarget&, const QString&)
        {
            return false;
        });

    dialog.arm_watch(0x4000, Kind::hardware_read_write, 4);
    CHECK_FALSE(dialog.watch_pending());
    CHECK_FALSE(dialog.isVisible());
    CHECK(controller.state() == Controller::State::idle);
    CHECK(controller.watch().state() == WatchState::idle);
    CHECK(dialog.status_text().contains(QStringLiteral("Attach cancelled")));
}

TEST_CASE("a live session arms the access watch without prompting", "[ui][access_watch]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::tests::FakeDebugBackend backend;
    backend.block_continue = true;
    Controller controller {backend};
    controller.start(42, "fake");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == Controller::State::running;
                       }));

    slopkit::process::AttachedTarget target = fake_target();
    AccessWatchDialog                dialog {controller, worker, target};

    int prompts = 0;
    dialog.debug_gate().set_attach_prompt(
        [&prompts](const slopkit::process::AttachedTarget&, const QString&)
        {
            ++prompts;
            return true;
        });

    dialog.arm_watch(0x4100, Kind::hardware_write, 4);
    CHECK(prompts == 0);
    CHECK_FALSE(dialog.watch_pending());
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           const auto* entry = watch_entry(controller);
                           return entry != nullptr && entry->armed;
                       }));
    CHECK(controller.watch().address() == 0x4100);
}

TEST_CASE("the access watch window renders its address lines in the embedded mono font", "[ui][access_watch]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::tests::FakeDebugBackend backend;
    Controller                       controller {backend};
    slopkit::process::AttachedTarget target = fake_target();
    AccessWatchDialog                dialog {controller, worker, target};

    auto* header = dialog.findChild<QLabel*>(QStringLiteral("access_watch_header"));
    auto* hint   = dialog.findChild<QLabel*>(QStringLiteral("access_watch_hint"));
    REQUIRE(header != nullptr);
    REQUIRE(hint != nullptr);

    const QString family = slopkit::ui::mono_font().family();
    CHECK(header->font().family() == family);
    CHECK(hint->font().family() == family);
}
