#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

namespace
{
    using slopkit::process::ProcessId;

    slopkit::ui::widgets::StatusLabel& address_status(slopkit::ui::MainWindow& window)
    {
        auto* status =
            window.statusBar()->findChild<slopkit::ui::widgets::StatusLabel*>(QStringLiteral("address_status"));
        REQUIRE(status != nullptr);
        return *status;
    }
}

TEST_CASE("loading a table auto attaches to its target process", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("auto_attach.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);

    const QString path = scratch_settings_file("auto_attach.skt");
    write_table_with_settings(path, "alpha", true, false);

    CHECK(address_list->load_table(path));
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return target.pid == 10 && target.session_live;
                    }));
    CHECK(target.name == "alpha");

    QCoreApplication::processEvents();
    auto* process_label = window.statusBar()->findChild<QLabel*>(QStringLiteral("process_label"));
    REQUIRE(process_label != nullptr);
    CHECK(process_label->text().contains(QStringLiteral("alpha")));

    auto* address_status =
        window.statusBar()->findChild<slopkit::ui::widgets::StatusLabel*>(QStringLiteral("address_status"));
    REQUIRE(address_status != nullptr);
    CHECK(address_list->findChildren<slopkit::ui::widgets::StatusLabel*>().isEmpty());
    CHECK(address_status->text().contains(QStringLiteral("Attached to alpha")));
}

TEST_CASE("a trainer auto attaches again after its target exits and returns", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("auto_attach_return.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);

    const QString path = scratch_settings_file("auto_attach_return.skt");
    write_table_with_settings(path, "alpha", true, false);

    CHECK(address_list->load_table(path));
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return target.pid == 10 && target.session_live;
                    }));

    access.processes.erase(std::remove_if(access.processes.begin(),
                                          access.processes.end(),
                                          [](const slopkit::process::ProcessInfo& info)
                                          {
                                              return info.pid == 10;
                                          }),
                           access.processes.end());

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return !target.valid();
                    }));

    access.processes = sample_processes();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return target.pid == 10 && target.session_live;
                    }));

    QCoreApplication::processEvents();
    auto* process_label = window.statusBar()->findChild<QLabel*>(QStringLiteral("process_label"));
    REQUIRE(process_label != nullptr);
    CHECK(process_label->text().contains(QStringLiteral("alpha")));

    auto* address_status =
        window.statusBar()->findChild<slopkit::ui::widgets::StatusLabel*>(QStringLiteral("address_status"));
    REQUIRE(address_status != nullptr);
    CHECK(address_list->findChildren<slopkit::ui::widgets::StatusLabel*>().isEmpty());
    CHECK(address_status->text().contains(QStringLiteral("Attached to alpha")));
}

TEST_CASE("an auto attach is skipped while a session is live", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("auto_attach_skip.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);

    const QString path = scratch_settings_file("auto_attach_skip.skt");
    write_table_with_settings(path, "alpha", true, false);

    CHECK(address_list->load_table(path));
    QCoreApplication::processEvents();

    CHECK(target.pid == 42);
    CHECK(access.list_calls.load() == 0);

    bool  reported = false;
    auto* address_status =
        window.statusBar()->findChild<slopkit::ui::widgets::StatusLabel*>(QStringLiteral("address_status"));
    REQUIRE(address_status != nullptr);
    if (address_status->text().contains(QStringLiteral("skipped")))
    {
        reported = true;
    }
    CHECK(reported);
}

TEST_CASE("a failed auto attach leaves the target detached", "[ui]")
{
    application();

    FakeAccess access;
    access.processes    = sample_processes();
    access.attach_fails = true;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("auto_attach_fail.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);

    const QString path = scratch_settings_file("auto_attach_fail.skt");
    write_table_with_settings(path, "alpha", true, false);

    CHECK(address_list->load_table(path));
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        auto* address_status = window.statusBar()->findChild<slopkit::ui::widgets::StatusLabel*>(
                            QStringLiteral("address_status"));
                        return address_status != nullptr
                            && address_status->text().contains(QStringLiteral("auto attach failed"));
                    }));
    CHECK_FALSE(target.valid());
}

TEST_CASE("a table can auto attach by executable path", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("auto_attach_path.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);

    const QString path = scratch_settings_file("auto_attach_path.skt");
    write_table_with_settings(path, "", true, true, "/usr/bin/beta");

    CHECK(address_list->load_table(path));
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return target.pid == 20 && target.session_live;
                    }));
    CHECK(target.name == "beta");
    CHECK(target.exe_path == "/usr/bin/beta");
}

TEST_CASE("a legacy name table keeps matching through the toggle", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("auto_attach_legacy.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);

    const QString path = scratch_settings_file("auto_attach_legacy.skt");
    write_table_with_settings(path, "alpha", true, true);

    CHECK(address_list->load_table(path));
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return target.pid == 10 && target.session_live;
                    }));
    CHECK(target.name == "alpha");
}

TEST_CASE("a path with the toggle off is not used for auto attach", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("auto_attach_path_off.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);

    const QString path = scratch_settings_file("auto_attach_path_off.skt");
    write_table_with_settings(path, "", true, false, "/usr/bin/beta");

    CHECK(address_list->load_table(path));
    QCoreApplication::processEvents();
    CHECK(target.pid == 0);
    CHECK(access.list_calls.load() == 0);
}

TEST_CASE("auto attach needs the flag and a target name", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();

    SECTION("the flag is off")
    {
        slopkit::process::AccessWorker   worker {access};
        slopkit::process::AttachedTarget target;
        slopkit::plugin::PluginHost      host;
        slopkit::ui::SettingsController  settings {scratch_settings_file("auto_attach_off.ini")};
        slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

        auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
        REQUIRE(address_list != nullptr);

        const QString path = scratch_settings_file("auto_attach_off.skt");
        write_table_with_settings(path, "alpha", false, false);
        CHECK(address_list->load_table(path));
        CHECK(target.pid == 0);
        CHECK(access.list_calls.load() == 0);
    }

    SECTION("the target name is empty")
    {
        slopkit::process::AccessWorker   worker {access};
        slopkit::process::AttachedTarget target;
        slopkit::plugin::PluginHost      host;
        slopkit::ui::SettingsController  settings {scratch_settings_file("auto_attach_noname.ini")};
        slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

        auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
        REQUIRE(address_list != nullptr);

        const QString path = scratch_settings_file("auto_attach_noname.skt");
        write_table_with_settings(path, "", true, false);
        CHECK(address_list->load_table(path));
        CHECK(target.pid == 0);
        CHECK(access.list_calls.load() == 0);
    }
}

TEST_CASE("a sandbox table launches a fresh practice target and attaches to it", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("sandbox_table.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);

    int launches = 0;
    window.set_sandbox_launcher(
        [&]
        {
            ++launches;
            access.processes.push_back(launched_sandbox(55));
            return std::expected<std::int64_t, std::string> {std::int64_t {55}};
        });

    bool prompted = false;
    window.set_sandbox_attach_prompt(
        [&](QWidget*, ProcessId)
        {
            prompted = true;
            return true;
        });

    const QString path = scratch_settings_file("sandbox_table.skt");
    write_table_with_settings(path, "slopkit-sandbox", true, false);

    CHECK(address_list->load_table(path));
    CHECK(address_status(window).text().contains(QStringLiteral("Practice target started (pid 55)")));

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return target.pid == 55 && target.session_live;
                    }));

    CHECK(launches == 1);
    CHECK_FALSE(prompted);
    CHECK(access.list_calls.load() == 1);
    CHECK(target.name == "slopkit-sandbox");
    CHECK(address_status(window).text().contains(QStringLiteral("Attached to slopkit-sandbox")));
}

TEST_CASE("a sandbox table always launches a fresh practice target", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    // A sandbox is already running, but the table must still get its own.
    access.processes.push_back(launched_sandbox(30));
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("sandbox_table_fresh.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);

    int launches = 0;
    window.set_sandbox_launcher(
        [&]
        {
            ++launches;
            access.processes.push_back(launched_sandbox(55));
            return std::expected<std::int64_t, std::string> {std::int64_t {55}};
        });

    const QString path = scratch_settings_file("sandbox_table_fresh.skt");
    write_table_with_settings(path, "slopkit-sandbox", true, false);

    CHECK(address_list->load_table(path));
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return target.pid == 55 && target.session_live;
                    }));

    CHECK(launches == 1);
}

TEST_CASE("a path-mode table naming the practice target launches it", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("sandbox_table_path.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);

    int launches = 0;
    window.set_sandbox_launcher(
        [&]
        {
            ++launches;
            access.processes.push_back(launched_sandbox(55));
            return std::expected<std::int64_t, std::string> {std::int64_t {55}};
        });

    const QString path = scratch_settings_file("sandbox_table_path.skt");
    write_table_with_settings(path, "", true, true, "/usr/lib/slopkit/slopkit-sandbox");

    CHECK(address_list->load_table(path));
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return target.pid == 55 && target.session_live;
                    }));

    CHECK(launches == 1);
}

TEST_CASE("a path-mode table naming another binary keeps the normal lookup", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("sandbox_table_path_other.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);

    int launches = 0;
    window.set_sandbox_launcher(
        [&]
        {
            ++launches;
            return std::expected<std::int64_t, std::string> {std::int64_t {55}};
        });

    const QString path = scratch_settings_file("sandbox_table_path_other.skt");
    write_table_with_settings(path, "", true, true, "/usr/bin/beta");

    CHECK(address_list->load_table(path));
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return target.pid == 20 && target.session_live;
                    }));

    CHECK(launches == 0);
}

TEST_CASE("a table naming a slopkit-like non-sandbox process attaches normally", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    auto other       = launched_sandbox(30);
    other.name       = "slopkit-sandbox-2";
    access.processes.push_back(other);
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("sandbox_table_other.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);

    int launches = 0;
    window.set_sandbox_launcher(
        [&]
        {
            ++launches;
            return std::expected<std::int64_t, std::string> {std::int64_t {55}};
        });

    const QString path = scratch_settings_file("sandbox_table_other.skt");
    write_table_with_settings(path, "slopkit-sandbox-2", true, false);

    CHECK(address_list->load_table(path));
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return target.pid == 30 && target.session_live;
                    }));

    CHECK(launches == 0);
}

TEST_CASE("a failed sandbox table launch reports the error without prompting", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("sandbox_table_fail.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);

    window.set_sandbox_launcher(
        []
        {
            return std::expected<std::int64_t, std::string> {
                std::unexpected(std::string {"slopkit-sandbox was not found"})};
        });

    bool prompted = false;
    window.set_sandbox_attach_prompt(
        [&](QWidget*, ProcessId)
        {
            prompted = true;
            return true;
        });

    const QString path = scratch_settings_file("sandbox_table_fail.skt");
    write_table_with_settings(path, "slopkit-sandbox", true, false);

    CHECK(address_list->load_table(path));
    QCoreApplication::processEvents();

    CHECK_FALSE(prompted);
    CHECK_FALSE(target.valid());
    CHECK(access.list_calls.load() == 0);
    CHECK(address_status(window).text().contains(QStringLiteral("slopkit-sandbox was not found")));
}

TEST_CASE("a launched practice target missing from the listing stays detached", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("sandbox_table_missing.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);

    // The launcher never injects the pid it returns into the listing.
    window.set_sandbox_launcher(
        []
        {
            return std::expected<std::int64_t, std::string> {std::int64_t {99}};
        });

    const QString path = scratch_settings_file("sandbox_table_missing.skt");
    write_table_with_settings(path, "slopkit-sandbox", true, false);

    CHECK(address_list->load_table(path));
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return address_status(window).text().contains(QStringLiteral("was not found"));
                    }));

    CHECK_FALSE(target.valid());
}
