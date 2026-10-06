#include <catch2/catch.hpp>

#include <QDir>

#include "support/ui_helpers.hpp"
#include "ui/table_file.hpp"

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
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

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
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

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
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

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
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller(), launch_path};

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
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

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
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

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
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

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
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

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
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

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
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    auto* address_status =
        window.statusBar()->findChild<slopkit::ui::widgets::StatusLabel*>(QStringLiteral("address_status"));
    REQUIRE(address_status != nullptr);
    CHECK(address_status->text().contains(QStringLiteral("Open failed")));

    CHECK(settings.values().last_table_path == missing);
}

TEST_CASE("a never-saved table is offered as the attached process name", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    target.pid          = 42;
    target.name         = "firefox";
    target.session_live = true;
    slopkit::table::AddressTable          table;
    slopkit::ui::panels::AddressListPanel panel {table, worker, target};

    // No remembered directory: the default table directory is proposed.
    CHECK(panel.suggested_table_path()
          == QDir(slopkit::ui::default_table_directory()).filePath(QStringLiteral("firefox.skt")));

    // A remembered directory wins over the default location.
    const QString remembered = QFileInfo(scratch_settings_file("save_prefill_dir.ini")).absolutePath();
    panel.set_dialog_directory(remembered);
    CHECK(panel.suggested_table_path() == QDir(remembered).filePath(QStringLiteral("firefox.skt")));
}

TEST_CASE("a never-saved table with a detached target falls back to untitled", "[ui]")
{
    application();

    FakeAccess                            access;
    slopkit::process::AccessWorker        worker {access};
    slopkit::process::AttachedTarget      target; // detached: no session, no pid
    slopkit::table::AddressTable          table;
    slopkit::ui::panels::AddressListPanel panel {table, worker, target};

    CHECK(panel.suggested_table_path()
          == QDir(slopkit::ui::default_table_directory()).filePath(QStringLiteral("untitled.skt")));
}

TEST_CASE("an existing table keeps its file name in the suggested save path", "[ui]")
{
    application();

    FakeAccess                            access;
    slopkit::process::AccessWorker        worker {access};
    slopkit::process::AttachedTarget      target;
    slopkit::table::AddressTable          table;
    slopkit::ui::panels::AddressListPanel panel {table, worker, target};

    const QString path = scratch_settings_file("existing_save_path.skt");
    panel.set_table_path(path);
    CHECK(panel.suggested_table_path() == path);

    // Only the directory may change; the file name survives.
    const QString remembered = QFileInfo(path).absolutePath();
    panel.set_dialog_directory(remembered);
    CHECK(panel.suggested_table_path() == QDir(remembered).filePath(QStringLiteral("existing_save_path.skt")));
}

TEST_CASE("saving a never-saved table proposes the target name and creates the default folder", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    target.pid          = 42;
    target.name         = "firefox";
    target.session_live = true;
    slopkit::table::AddressTable          table;
    slopkit::ui::panels::AddressListPanel panel {table, worker, target};

    const QString directory = slopkit::ui::default_table_directory();
    const bool    existed   = QDir(directory).exists();

    const QString chosen = scratch_settings_file("default_dir_save.skt");
    QString       captured;
    panel.set_save_path_prompt(
        [&](QWidget*, const QString& suggested)
        {
            captured = suggested;
            return std::optional<QString> {chosen};
        });
    panel.save_table_as();

    CHECK(captured == QDir(directory).filePath(QStringLiteral("firefox.skt")));
    CHECK(QDir(directory).exists()); // the first save can start in the default folder
    CHECK(QFileInfo::exists(chosen));
    CHECK(panel.table_path() == chosen);

    // Leave the user's home as we found it: drop the scratch file and remove the
    // default folder only when this case had to create it.
    std::error_code error;
    std::filesystem::remove(chosen.toStdString(), error);
    if (!existed)
    {
        QDir().rmdir(directory);
    }
}

TEST_CASE("a cancelled save chooser writes nothing and leaves the settings", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    target.pid          = 42;
    target.name         = "firefox";
    target.session_live = true;
    slopkit::plugin::PluginHost     host;
    slopkit::ui::SettingsController settings {scratch_settings_file("save_cancel.ini")};
    const QString                   remembered = QFileInfo(scratch_settings_file("save_cancel_dir.ini")).absolutePath();
    settings.set_last_directory(remembered);
    slopkit::ui::MainWindow window {worker, target, host, settings, shared_debug_controller()};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);

    address_list->set_save_path_prompt(
        [](QWidget*, const QString&)
        {
            return std::nullopt;
        });
    address_list->save_table(); // a cancelled chooser must not create the table file

    CHECK(address_list->table_path().isEmpty());
    CHECK(settings.values().last_table_path.isEmpty());
    CHECK(settings.values().last_directory == remembered);
}

TEST_CASE("saving into a missing directory reports a failure", "[ui]")
{
    application();

    FakeAccess                            access;
    slopkit::process::AccessWorker        worker {access};
    slopkit::process::AttachedTarget      target;
    slopkit::table::AddressTable          table;
    slopkit::ui::panels::AddressListPanel panel {table, worker, target};

    // A remembered directory keeps the default folder out of the flow.
    const QString remembered = QFileInfo(scratch_settings_file("save_missing_dir.ini")).absolutePath();
    panel.set_dialog_directory(remembered);

    QString message;
    bool    is_error = false;
    QObject::connect(&panel,
                     &slopkit::ui::panels::AddressListPanel::statusChanged,
                     [&](const QString& text, bool error)
                     {
                         message  = text;
                         is_error = error;
                     });

    const QString missing = QDir(remembered).filePath(QStringLiteral("no_such_dir/table.skt"));
    panel.set_save_path_prompt(
        [&](QWidget*, const QString&)
        {
            return std::optional<QString> {missing};
        });
    panel.save_table_as();

    CHECK(is_error);
    CHECK(message.contains(QStringLiteral("Save failed")));
}

TEST_CASE("a never-saved table saved through the window is remembered in the settings", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    target.pid          = 42;
    target.name         = "firefox";
    target.session_live = true;
    slopkit::plugin::PluginHost     host;
    slopkit::ui::SettingsController settings {scratch_settings_file("save_new_table.ini")};
    // A remembered directory keeps the default location out of the flow.
    const QString                   remembered = QFileInfo(scratch_settings_file("save_new_dir.ini")).absolutePath();
    settings.set_last_directory(remembered);
    slopkit::ui::MainWindow window {worker, target, host, settings, shared_debug_controller()};

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);

    const QString chosen = scratch_settings_file("save_new_table.skt");
    QString       captured;
    address_list->set_save_path_prompt(
        [&](QWidget*, const QString& suggested)
        {
            captured = suggested;
            return std::optional<QString> {chosen};
        });

    address_list->save_table(); // Ctrl+S on a table that was never saved

    CHECK(captured == QDir(remembered).filePath(QStringLiteral("firefox.skt")));
    CHECK(QFileInfo::exists(chosen));
    CHECK(address_list->table_path() == chosen);
    CHECK(settings.values().last_table_path == chosen);
    CHECK(settings.values().last_directory == QFileInfo(chosen).absolutePath());
}
