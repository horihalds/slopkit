#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

TEST_CASE("loading a table reports the entry count and emits tableLoaded", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::table::AddressTable     table;

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};

    int loaded_signals = 0;
    QObject::connect(&panel,
                     &slopkit::ui::panels::AddressListPanel::tableLoaded,
                     [&loaded_signals]
                     {
                         ++loaded_signals;
                     });

    const QString path = scratch_settings_file("load_seam.skt");
    {
        slopkit::table::AddressTable writer;
        slopkit::table::AddressEntry entry;
        entry.address = 0x1040;
        entry.type    = slopkit::scan::ValueType::int32;
        entry.bytes   = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
        writer.add(entry);
        REQUIRE(slopkit::table::save(std::filesystem::path(path.toStdString()), writer).has_value());
    }

    CHECK(panel.load_table(path));
    CHECK(table.size() == 1);
    CHECK(loaded_signals == 1);

    // An unreadable path fails without emitting the signal.
    CHECK_FALSE(panel.load_table(QStringLiteral("/nonexistent/slopkit/table.skt")));
    CHECK(loaded_signals == 1);
}

TEST_CASE("a table load is remembered in the settings", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("remember_table.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);

    const QString path = scratch_settings_file("remember_table.skt");
    write_table_with_settings(path, "alpha", false, false);
    REQUIRE(address_list->load_table(path));

    CHECK(address_list->table_path() == path);
    CHECK(settings.values().last_table_path == path);
    CHECK(settings.values().last_directory == QFileInfo(path).absolutePath());
}

TEST_CASE("a log save is remembered in the settings", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("remember_log.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings};

    auto* log = window.findChild<slopkit::ui::dialogs::LogDialog*>();
    REQUIRE(log != nullptr);

    const QString path = scratch_settings_file("saved.log");
    log->save_to(path);

    CHECK(QFileInfo::exists(path));
    CHECK(settings.values().last_directory == QFileInfo(path).absolutePath());
}

TEST_CASE("the auto-load switch opens the remembered table at start-up", "[ui]")
{
    application();

    const QString table_path = scratch_settings_file("autoload.skt");
    {
        slopkit::table::AddressTable writer;
        slopkit::table::AddressEntry entry;
        entry.address = 0x2000;
        entry.type    = slopkit::scan::ValueType::int32;
        entry.bytes   = {std::byte {7}, std::byte {0}, std::byte {0}, std::byte {0}};
        writer.add(entry);
        REQUIRE(slopkit::table::save(std::filesystem::path(table_path.toStdString()), writer).has_value());
    }

    const QString settings_path = scratch_settings_file("autoload.ini");
    {
        slopkit::ui::SettingsController seed {settings_path};
        seed.set_auto_load_last_table(true);
        seed.set_last_table_path(table_path);
    }

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {settings_path};
    slopkit::ui::MainWindow          window {worker, target, host, settings};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);
    auto* view = address_list->findChild<QTableView*>();
    REQUIRE(view != nullptr);
    REQUIRE(view->model() != nullptr);
    CHECK(view->model()->rowCount() == 1);
}

TEST_CASE("a launch table path opens and skips the remembered auto-load", "[ui]")
{
    application();

    // The remembered table must NOT be loaded once an explicit launch path exists.
    const QString remembered_path = scratch_settings_file("launch_remembered.skt");
    write_table_with_settings(remembered_path, "remembered-target", false, false);

    const QString settings_path = scratch_settings_file("launch_table.ini");
    {
        slopkit::ui::SettingsController seed {settings_path};
        seed.set_auto_load_last_table(true);
        seed.set_last_table_path(remembered_path);
    }

    const QString launch_path = scratch_settings_file("launch_table.skt");
    {
        slopkit::table::AddressTable writer;
        writer.settings().target_process = "launch-target";
        slopkit::table::AddressEntry entry;
        entry.address     = 0x1040;
        entry.description = "launch-row";
        entry.type        = slopkit::scan::ValueType::int32;
        entry.bytes       = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
        writer.add(entry);
        REQUIRE(slopkit::table::save(std::filesystem::path(launch_path.toStdString()), writer).has_value());
    }

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {settings_path};
    slopkit::ui::MainWindow          window {worker, target, host, settings, launch_path};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);
    CHECK(address_list->table_path() == launch_path);

    auto* view = address_list->findChild<QTableView*>();
    REQUIRE(view != nullptr);
    REQUIRE(view->model() != nullptr);
    REQUIRE(view->model()->rowCount() == 1);
    CHECK(view->model()->index(0, 0).data(Qt::DisplayRole).toString() == QStringLiteral("launch-row"));
}

TEST_CASE("opening into an empty table loads without prompting", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("open_empty.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);

    int prompt_calls = 0;
    window.set_table_conflict_prompt(
        [&prompt_calls](QWidget*, const slopkit::ui::dialogs::TableConflictInfo&)
        {
            ++prompt_calls;
            return slopkit::ui::dialogs::TableConflictChoice::merge;
        });

    const QString path = scratch_settings_file("open_empty.skt");
    write_entry_table(path, QStringLiteral("only-row"), 0x1000, "empty-target");

    CHECK(window.open_table_request(path));
    CHECK(prompt_calls == 0);
    CHECK(address_list->table_path() == path);

    auto* view = address_list->findChild<QTableView*>();
    REQUIRE(view != nullptr);
    REQUIRE(view->model() != nullptr);
    REQUIRE(view->model()->rowCount() == 1);
    CHECK(view->model()->index(0, 0).data(Qt::DisplayRole).toString() == QStringLiteral("only-row"));

    // A file that cannot be read reports and never prompts.
    auto* address_status =
        window.statusBar()->findChild<slopkit::ui::widgets::StatusLabel*>(QStringLiteral("address_status"));
    REQUIRE(address_status != nullptr);
    CHECK_FALSE(window.open_table_request(QStringLiteral("/nonexistent/slopkit/missing.skt")));
    CHECK(prompt_calls == 0);
    CHECK(address_status->text().contains(QStringLiteral("Open failed")));
    REQUIRE(view->model()->rowCount() == 1);
    CHECK(view->model()->index(0, 0).data(Qt::DisplayRole).toString() == QStringLiteral("only-row"));
}

TEST_CASE("cancelling an open request keeps the table, path and settings", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("conflict_cancel.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);

    const QString base_path     = scratch_settings_file("conflict_cancel_base.skt");
    const QString incoming_path = scratch_settings_file("conflict_cancel_incoming.skt");
    write_entry_table(base_path, QStringLiteral("base-row"), 0x1000, "base-target");
    write_entry_table(incoming_path, QStringLiteral("incoming-row"), 0x2000, "incoming-target");
    REQUIRE(window.open_table_request(base_path));

    slopkit::ui::dialogs::TableConflictInfo seen;
    int                                     prompt_calls = 0;
    window.set_table_conflict_prompt(
        [&prompt_calls, &seen](QWidget*, const slopkit::ui::dialogs::TableConflictInfo& info)
        {
            ++prompt_calls;
            seen = info;
            return slopkit::ui::dialogs::TableConflictChoice::cancel;
        });

    CHECK_FALSE(window.open_table_request(incoming_path));
    CHECK(prompt_calls == 1);
    CHECK(seen.incoming_path == incoming_path);
    CHECK(seen.incoming_entries == 1);
    CHECK(seen.current_path == base_path);
    CHECK(seen.current_entries == 1);

    CHECK(address_list->table_path() == base_path);
    auto* view = address_list->findChild<QTableView*>();
    REQUIRE(view != nullptr);
    REQUIRE(view->model() != nullptr);
    REQUIRE(view->model()->rowCount() == 1);
    CHECK(view->model()->index(0, 0).data(Qt::DisplayRole).toString() == QStringLiteral("base-row"));

    auto* address_status =
        window.statusBar()->findChild<slopkit::ui::widgets::StatusLabel*>(QStringLiteral("address_status"));
    REQUIRE(address_status != nullptr);
    CHECK(address_status->text().contains(QStringLiteral("Open cancelled")));
}

TEST_CASE("overwriting replaces the open table, path and settings", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("conflict_overwrite.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);

    const QString base_path     = scratch_settings_file("conflict_overwrite_base.skt");
    const QString incoming_path = scratch_settings_file("conflict_overwrite_incoming.skt");
    write_entry_table(base_path, QStringLiteral("base-row"), 0x1000, "base-target");
    write_entry_table(incoming_path, QStringLiteral("incoming-row"), 0x2000, "incoming-target");
    REQUIRE(window.open_table_request(base_path));

    int loaded = 0;
    QObject::connect(address_list,
                     &slopkit::ui::panels::AddressListPanel::tableLoaded,
                     [&loaded]
                     {
                         ++loaded;
                     });
    window.set_table_conflict_prompt(
        [](QWidget*, const slopkit::ui::dialogs::TableConflictInfo&)
        {
            return slopkit::ui::dialogs::TableConflictChoice::overwrite;
        });

    CHECK(window.open_table_request(incoming_path));
    CHECK(loaded == 1);
    CHECK(address_list->table_path() == incoming_path);

    auto* view = address_list->findChild<QTableView*>();
    REQUIRE(view != nullptr);
    REQUIRE(view->model() != nullptr);
    REQUIRE(view->model()->rowCount() == 1);
    CHECK(view->model()->index(0, 0).data(Qt::DisplayRole).toString() == QStringLiteral("incoming-row"));
}

TEST_CASE("merging an open request keeps the path and adds only new rows", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {scratch_settings_file("conflict_merge.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);

    const QString base_path     = scratch_settings_file("conflict_merge_base.skt");
    const QString incoming_path = scratch_settings_file("conflict_merge_incoming.skt");
    {
        slopkit::table::AddressTable table;
        table.settings().target_process = "base-target";
        slopkit::table::AddressEntry shared;
        shared.address     = 0x1000;
        shared.description = "shared-row";
        shared.type        = slopkit::scan::ValueType::int32;
        shared.bytes       = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
        table.add(shared);
        REQUIRE(slopkit::table::save(std::filesystem::path(base_path.toStdString()), table).has_value());
    }
    {
        slopkit::table::AddressTable table;
        table.settings().target_process = "incoming-target";
        slopkit::table::AddressEntry shared;
        shared.address     = 0x1000; // matches the open table, so it is skipped
        shared.description = "shared-row";
        shared.type        = slopkit::scan::ValueType::int32;
        shared.bytes       = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
        slopkit::table::AddressEntry fresh;
        fresh.address     = 0x2000;
        fresh.description = "fresh-row";
        fresh.type        = slopkit::scan::ValueType::int32;
        fresh.bytes       = {std::byte {2}, std::byte {0}, std::byte {0}, std::byte {0}};
        table.add(shared);
        table.add(fresh);
        REQUIRE(slopkit::table::save(std::filesystem::path(incoming_path.toStdString()), table).has_value());
    }

    REQUIRE(window.open_table_request(base_path));

    int loaded = 0;
    QObject::connect(address_list,
                     &slopkit::ui::panels::AddressListPanel::tableLoaded,
                     [&loaded]
                     {
                         ++loaded;
                     });
    window.set_table_conflict_prompt(
        [](QWidget*, const slopkit::ui::dialogs::TableConflictInfo&)
        {
            return slopkit::ui::dialogs::TableConflictChoice::merge;
        });

    CHECK(window.open_table_request(incoming_path));
    CHECK(loaded == 0); // a merge must not trigger the table-driven auto attach
    CHECK(address_list->table_path() == base_path);

    auto* view = address_list->findChild<QTableView*>();
    REQUIRE(view != nullptr);
    REQUIRE(view->model() != nullptr);
    REQUIRE(view->model()->rowCount() == 2);
    CHECK(view->model()->index(0, 0).data(Qt::DisplayRole).toString() == QStringLiteral("shared-row"));
    CHECK(view->model()->index(1, 0).data(Qt::DisplayRole).toString() == QStringLiteral("fresh-row"));

    // Re-merging the same file changes nothing.
    CHECK(window.open_table_request(incoming_path));
    REQUIRE(view->model()->rowCount() == 2);
}

TEST_CASE("adopting a parsed table takes the file's settings", "[ui]")
{
    application();

    FakeAccess                            access;
    slopkit::process::AccessWorker        worker {access};
    slopkit::process::AttachedTarget      target;
    slopkit::table::AddressTable          table;
    slopkit::ui::panels::AddressListPanel panel {table, worker, target};

    const QString path = scratch_settings_file("adopt_settings.skt");
    write_entry_table(path, QStringLiteral("row"), 0x1000, "incoming-target");

    auto parsed = panel.parse_table(path);
    REQUIRE(parsed.has_value());
    CHECK(parsed->settings().target_process == "incoming-target");

    panel.adopt_table(path, std::move(*parsed));
    CHECK(table.settings().target_process == "incoming-target");
    CHECK(panel.table_path() == path);
}

TEST_CASE("merging keeps the current table settings and path", "[ui]")
{
    application();

    FakeAccess                            access;
    slopkit::process::AccessWorker        worker {access};
    slopkit::process::AttachedTarget      target;
    slopkit::table::AddressTable          table;
    slopkit::ui::panels::AddressListPanel panel {table, worker, target};

    const QString base_path = scratch_settings_file("merge_settings_base.skt");
    write_entry_table(base_path, QStringLiteral("base-row"), 0x1000, "base-target");
    REQUIRE(panel.load_table(base_path));

    slopkit::table::AddressTable incoming;
    incoming.settings().target_process = "incoming-target";
    slopkit::table::AddressEntry fresh;
    fresh.address     = 0x2000;
    fresh.description = "fresh-row";
    fresh.type        = slopkit::scan::ValueType::int32;
    fresh.bytes       = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
    incoming.add(fresh);

    const auto summary = panel.merge_table(incoming);
    CHECK(summary.added == 1);
    CHECK(summary.skipped == 0);
    CHECK(table.size() == 2);
    CHECK(table.settings().target_process == "base-target");
    CHECK(panel.table_path() == base_path);
}

TEST_CASE("the auto-load switch off leaves the table empty", "[ui]")
{
    application();

    const QString table_path = scratch_settings_file("autoload_off.skt");
    write_table_with_settings(table_path, "alpha", false, false);

    const QString settings_path = scratch_settings_file("autoload_off.ini");
    {
        slopkit::ui::SettingsController seed {settings_path};
        seed.set_auto_load_last_table(false);
        seed.set_last_table_path(table_path);
    }

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {settings_path};
    slopkit::ui::MainWindow          window {worker, target, host, settings};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);
    CHECK(address_list->table_path() == table_path); // remembered for a later save

    auto* view = address_list->findChild<QTableView*>();
    REQUIRE(view != nullptr);
    REQUIRE(view->model() != nullptr);
    CHECK(view->model()->rowCount() == 0);
}

TEST_CASE("a failed auto-load reports and keeps the remembered path", "[ui]")
{
    application();

    const QString missing       = QStringLiteral("/nonexistent/slopkit/missing.skt");
    const QString settings_path = scratch_settings_file("autoload_fail.ini");
    {
        slopkit::ui::SettingsController seed {settings_path};
        seed.set_auto_load_last_table(true);
        seed.set_last_table_path(missing);
    }

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::plugin::PluginHost      host;
    slopkit::ui::SettingsController  settings {settings_path};
    slopkit::ui::MainWindow          window {worker, target, host, settings};

    auto* address_status =
        window.statusBar()->findChild<slopkit::ui::widgets::StatusLabel*>(QStringLiteral("address_status"));
    REQUIRE(address_status != nullptr);
    CHECK(address_status->text().contains(QStringLiteral("Open failed")));

    CHECK(settings.values().last_table_path == missing);
}
