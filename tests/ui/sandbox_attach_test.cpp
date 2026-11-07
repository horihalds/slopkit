#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

namespace
{
    using slopkit::process::ProcessId;

    slopkit::ui::MainWindow::SandboxLauncher launcher_for(std::int64_t pid)
    {
        return [pid]
        {
            return std::expected<std::int64_t, std::string> {pid};
        };
    }

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
}

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

TEST_CASE("exiting while attached to the table's practice target stops it", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("sandbox_shutdown_attached.ini")};

    std::optional<std::int64_t> stopped;
    {
        slopkit::ui::MainWindow window {worker, target, host, settings, shared_debug_controller()};

        auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
        REQUIRE(address_list != nullptr);

        window.set_sandbox_launcher(
            [&]
            {
                access.processes.push_back(launched_sandbox(55));
                return std::expected<std::int64_t, std::string> {std::int64_t {55}};
            });
        window.set_sandbox_terminator(
            [&](std::int64_t pid)
            {
                stopped = pid;
                return std::expected<void, std::string> {};
            });

        const QString path = scratch_settings_file("sandbox_shutdown_attached.skt");
        write_table_with_settings(path, "slopkit-sandbox", true, false);

        CHECK(address_list->load_table(path));
        REQUIRE(pump_ui(worker,
                        [&]
                        {
                            return target.pid == 55 && target.session_live;
                        }));
    }

    CHECK(stopped == std::optional<std::int64_t> {55});
}

TEST_CASE("exiting while attached to another process leaves it alone", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("sandbox_shutdown_other.ini")};

    std::optional<std::int64_t> stopped;
    {
        slopkit::ui::MainWindow window {worker, target, host, settings, shared_debug_controller()};

        auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
        REQUIRE(address_list != nullptr);

        window.set_sandbox_terminator(
            [&](std::int64_t pid)
            {
                stopped = pid;
                return std::expected<void, std::string> {};
            });

        const QString path = scratch_settings_file("sandbox_shutdown_other.skt");
        write_table_with_settings(path, "alpha", true, false);

        CHECK(address_list->load_table(path));
        REQUIRE(pump_ui(worker,
                        [&]
                        {
                            return target.pid == 10 && target.session_live;
                        }));
    }

    CHECK_FALSE(stopped.has_value());
}

TEST_CASE("a Help-menu practice target survives slopkit", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("sandbox_shutdown_help.ini")};

    std::optional<std::int64_t> stopped;
    {
        slopkit::ui::MainWindow window {worker, target, host, settings, shared_debug_controller()};

        window.set_sandbox_launcher(launcher_for(20));
        window.set_sandbox_attach_prompt(
            [](QWidget*, ProcessId)
            {
                return true;
            });
        window.set_sandbox_terminator(
            [&](std::int64_t pid)
            {
                stopped = pid;
                return std::expected<void, std::string> {};
            });

        practice_target_action(window)->trigger();
        REQUIRE(pump_ui(worker,
                        [&]
                        {
                            return target.pid == 20 && target.session_live;
                        }));
    }

    CHECK_FALSE(stopped.has_value());
}

TEST_CASE("exiting while detached stops nothing", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("sandbox_shutdown_detached.ini")};

    std::optional<std::int64_t> stopped;
    {
        slopkit::ui::MainWindow window {worker, target, host, settings, shared_debug_controller()};
        window.set_sandbox_terminator(
            [&](std::int64_t pid)
            {
                stopped = pid;
                return std::expected<void, std::string> {};
            });
    }

    CHECK_FALSE(stopped.has_value());
}

TEST_CASE("a failing termination does not stop slopkit from shutting down", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("sandbox_shutdown_fail.ini")};

    int terminations = 0;
    {
        slopkit::ui::MainWindow window {worker, target, host, settings, shared_debug_controller()};

        auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
        REQUIRE(address_list != nullptr);

        window.set_sandbox_launcher(
            [&]
            {
                access.processes.push_back(launched_sandbox(55));
                return std::expected<std::int64_t, std::string> {std::int64_t {55}};
            });
        window.set_sandbox_terminator(
            [&](std::int64_t)
            {
                ++terminations;
                return std::expected<void, std::string> {std::unexpected(std::string {"kill failed"})};
            });

        const QString path = scratch_settings_file("sandbox_shutdown_fail.skt");
        write_table_with_settings(path, "slopkit-sandbox", true, false);

        CHECK(address_list->load_table(path));
        REQUIRE(pump_ui(worker,
                        [&]
                        {
                            return target.pid == 55 && target.session_live;
                        }));
    }

    CHECK(terminations == 1);
}
