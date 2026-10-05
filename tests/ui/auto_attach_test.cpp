#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

TEST_CASE("loading a table auto attaches to its target process", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("auto_attach.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings};

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

TEST_CASE("an auto attach is skipped while a session is live", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("auto_attach_skip.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);

    const QString path = scratch_settings_file("auto_attach_skip.skt");
    write_table_with_settings(path, "alpha", true, false);

    CHECK(address_list->load_table(path));
    QCoreApplication::processEvents();

    // The live session is kept and the skip is reported.
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
    slopkit::ui::MainWindow          window {worker, target, host, settings};

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
    slopkit::ui::MainWindow          window {worker, target, host, settings};

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
    slopkit::ui::MainWindow          window {worker, target, host, settings};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);

    // A ticked toggle without a path keeps the pre-exe_path behaviour.
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
    slopkit::ui::MainWindow          window {worker, target, host, settings};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);

    // The toggle is off, so the empty name is the selected field: no attach.
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
        slopkit::ui::MainWindow          window {worker, target, host, settings};

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
        slopkit::ui::MainWindow          window {worker, target, host, settings};

        auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
        REQUIRE(address_list != nullptr);

        const QString path = scratch_settings_file("auto_attach_noname.skt");
        write_table_with_settings(path, "", true, false);
        CHECK(address_list->load_table(path));
        CHECK(target.pid == 0);
        CHECK(access.list_calls.load() == 0);
    }
}
