#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

namespace
{
    using slopkit::process::ProcessId;

    // A launcher that reports `pid` as the just-started practice target.
    slopkit::ui::MainWindow::SandboxLauncher launcher_for(std::int64_t pid)
    {
        return [pid]
        {
            return std::expected<std::int64_t, std::string> {pid};
        };
    }

    // The Help > Launch Practice Target action.
    QAction* practice_target_action(slopkit::ui::MainWindow& window)
    {
        const QList<QAction*> menus = window.menuBar()->actions();
        REQUIRE(menus.size() == 3);
        REQUIRE(menus[2]->text() == QStringLiteral("Help"));
        const QList<QAction*> entries = menus[2]->menu()->actions();
        REQUIRE_FALSE(entries.isEmpty());
        return entries.first();
    }

    slopkit::ui::widgets::StatusLabel& address_status(slopkit::ui::MainWindow& window)
    {
        auto* status =
            window.statusBar()->findChild<slopkit::ui::widgets::StatusLabel*>(QStringLiteral("address_status"));
        REQUIRE(status != nullptr);
        return *status;
    }
} // namespace

TEST_CASE("declining the practice-target attach question leaves the target detached", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("sandbox_decline.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    window.set_sandbox_launcher(launcher_for(20));

    bool asked = false;
    window.set_sandbox_attach_prompt(
        [&](QWidget*, ProcessId)
        {
            asked = true;
            return false;
        });

    practice_target_action(window)->trigger();
    QCoreApplication::processEvents();

    CHECK(asked);
    CHECK_FALSE(target.valid());
    // No question was answered with Yes, so no listing was ever submitted.
    CHECK(access.list_calls.load() == 0);
    CHECK(address_status(window).text().contains(QStringLiteral("Practice target started (pid 20)")));
}

TEST_CASE("accepting the practice-target attach question attaches to the launched pid", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("sandbox_accept.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    // The sandbox reports pid 20, which the listing carries as "beta".
    window.set_sandbox_launcher(launcher_for(20));

    std::optional<ProcessId> asked_pid;
    window.set_sandbox_attach_prompt(
        [&](QWidget*, ProcessId pid)
        {
            asked_pid = pid;
            return true;
        });

    practice_target_action(window)->trigger();

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return target.pid == 20 && target.session_live;
                    }));
    REQUIRE(asked_pid.has_value());
    CHECK(*asked_pid == ProcessId {20});
    CHECK(target.name == "beta");
    CHECK(address_status(window).text().contains(QStringLiteral("Attached to beta")));
}

TEST_CASE("a failed practice-target launch never asks to attach", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("sandbox_launch_fail.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    window.set_sandbox_launcher(
        []
        {
            return std::expected<std::int64_t, std::string> {
                std::unexpected(std::string {"slopkit-sandbox was not found"})};
        });

    bool asked = false;
    window.set_sandbox_attach_prompt(
        [&](QWidget*, ProcessId)
        {
            asked = true;
            return true;
        });

    practice_target_action(window)->trigger();
    QCoreApplication::processEvents();

    CHECK_FALSE(asked);
    CHECK_FALSE(target.valid());
    CHECK(address_status(window).text().contains(QStringLiteral("slopkit-sandbox was not found")));
}

TEST_CASE("a launched pid missing from the listing is reported and stays detached", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("sandbox_missing.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    // The listing carries only pids 10 and 20, never the launched 99.
    window.set_sandbox_launcher(launcher_for(99));
    window.set_sandbox_attach_prompt(
        [](QWidget*, ProcessId)
        {
            return true;
        });

    practice_target_action(window)->trigger();

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return address_status(window).text().contains(QStringLiteral("was not found"));
                    }));
    CHECK_FALSE(target.valid());
}
