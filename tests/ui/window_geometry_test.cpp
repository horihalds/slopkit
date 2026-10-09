#include <catch2/catch.hpp>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QSettings>
#include <QWidget>

#include "support/ui_helpers.hpp"
#include "ui/components/window_geometry.hpp"

namespace
{
    using slopkit::ui::SettingsController;
    using slopkit::ui::WindowGeometryKeeper;
    using slopkit::ui::WindowId;

    std::string read_file(const QString& path)
    {
        std::ifstream file(path.toStdString(), std::ios::binary);
        return std::string {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    std::size_t count_occurrences(const std::string& text, const std::string& needle)
    {
        std::size_t count = 0;
        for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        {
            ++count;
        }
        return count;
    }

    // Writes a geometry blob under `key` into the scratch INI with the raw
    // QSettings API, so the seed does not go through SettingsController.
    QByteArray seed_window_geometry(const QString& path, const char* key)
    {
        QWidget seed;
        seed.resize(360, 260);
        const QByteArray blob = seed.saveGeometry();
        REQUIRE_FALSE(blob.isEmpty());
        QSettings writer {path, QSettings::IniFormat};
        writer.setValue(QString::fromLatin1(key), blob);
        writer.sync();
        return blob;
    }

    // Where a window of `window`'s size lands when centred on `parent`'s frame.
    QPoint centred_on(const QWidget& parent, const QWidget& window)
    {
        const QSize own = window.size();
        return parent.frameGeometry().center() - QPoint(own.width() / 2, own.height() / 2);
    }

    // Triggers a table-area command through the address list's own menu, the
    // route the removed scanner buttons used to carry.
    void trigger_table_command(slopkit::ui::MainWindow& window, const QString& text)
    {
        auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
        REQUIRE(address_list != nullptr);
        QMenu menu;
        address_list->populate_panel_menu(menu);
        QAction* action = nullptr;
        for (QAction* candidate : menu.actions())
        {
            if (candidate->text() == text)
            {
                action = candidate;
            }
        }
        REQUIRE(action != nullptr);
        action->trigger();
    }
} // namespace

TEST_CASE("a keeper restores a pre-seeded geometry", "[window_geometry]")
{
    application();

    const QString      path = scratch_settings_file("keeper_restore.ini");
    SettingsController settings {path};

    QWidget seed;
    seed.resize(360, 260);
    const QByteArray blob = seed.saveGeometry();
    REQUIRE_FALSE(blob.isEmpty());
    settings.set_window_geometry(WindowId::log, blob);

    QWidget window;
    window.resize(120, 90);
    WindowGeometryKeeper keeper {window, settings, WindowId::log};

    CHECK(window.size() == QSize(360, 260));
}

TEST_CASE("hiding a window stores its geometry in the INI file", "[window_geometry]")
{
    application();

    const QString      path = scratch_settings_file("keeper_store.ini");
    SettingsController settings {path};

    QWidget              window;
    WindowGeometryKeeper keeper {window, settings, WindowId::log};

    window.resize(400, 300);
    window.show();
    QCoreApplication::processEvents();
    window.hide();
    QCoreApplication::processEvents();

    CHECK_FALSE(settings.window_geometry(WindowId::log).isEmpty());

    SettingsController reloaded {path};
    CHECK_FALSE(reloaded.window_geometry(WindowId::log).isEmpty());
}

TEST_CASE("a rejected geometry logs one warning and keeps the default", "[window_geometry]")
{
    application();

    const QString      path = scratch_settings_file("keeper_reject.ini");
    SettingsController settings {path};
    settings.set_window_geometry(WindowId::log, QByteArray {"not-a-geometry"});

    std::vector<slopkit::log::Record> records;
    SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                            {
                        records.push_back(record);
                                            }};

    QWidget window;
    window.resize(200, 150);
    WindowGeometryKeeper keeper {window, settings, WindowId::log};

    CHECK(window.size() == QSize(200, 150));

    int warnings = 0;
    for (const slopkit::log::Record& record : records)
    {
        if (record.level == slopkit::log::Level::warning && record.category == "app")
        {
            ++warnings;
        }
    }
    CHECK(warnings == 1);
}

TEST_CASE("closing then hiding a window stores one geometry key only", "[window_geometry]")
{
    application();

    const QString      path = scratch_settings_file("keeper_close_hide.ini");
    SettingsController settings {path};

    QWidget              window;
    WindowGeometryKeeper keeper {window, settings, WindowId::log};

    window.resize(320, 240);
    window.show();
    QCoreApplication::processEvents();
    window.close();
    window.hide();
    QCoreApplication::processEvents();

    CHECK_FALSE(settings.window_geometry(WindowId::log).isEmpty());

    const std::string text = read_file(path);
    CHECK(count_occurrences(text, "log_geometry=") == 1);
}

TEST_CASE("the Log dialog reopens at its remembered size", "[window_geometry]")
{
    application();

    const QString path = scratch_settings_file("log_geometry.ini");
    QSize         saved_size;
    QPoint        saved_position;
    {
        slopkit::plugin::PluginHost      host;
        slopkit::process::PluginAccess   access {host};
        slopkit::process::AccessWorker   worker {access};
        slopkit::process::AttachedTarget target;
        slopkit::ui::SettingsController  settings {path};
        slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

        QAction* log_action = window.menuBar()->actions().at(1)->menu()->actions().first();
        REQUIRE(log_action->text() == QStringLiteral("Log"));
        log_action->trigger();
        auto* log_dialog = window.findChild<slopkit::ui::dialogs::LogDialog*>();
        REQUIRE(log_dialog != nullptr);

        log_dialog->resize(700, 640);
        log_dialog->show();
        QCoreApplication::processEvents();
        log_dialog->move(30, 40);
        QCoreApplication::processEvents();
        // The offscreen platform may clamp an over-wide request, so the size to
        // restore is whatever the dialog actually holds when it is hidden.
        saved_size     = log_dialog->size();
        saved_position = log_dialog->pos();
        CHECK(saved_size != QSize(760, 520));
        log_dialog->hide();
        QCoreApplication::processEvents();

        CHECK_FALSE(settings.window_geometry(WindowId::log).isEmpty());
    }

    {
        slopkit::plugin::PluginHost      host;
        slopkit::process::PluginAccess   access {host};
        slopkit::process::AccessWorker   worker {access};
        slopkit::process::AttachedTarget target;
        slopkit::ui::SettingsController  settings {path};
        slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

        QAction* log_action = window.menuBar()->actions().at(1)->menu()->actions().first();
        log_action->trigger();
        auto* log_dialog = window.findChild<slopkit::ui::dialogs::LogDialog*>();
        REQUIRE(log_dialog != nullptr);
        CHECK(log_dialog->size() == saved_size);
        CHECK(log_dialog->pos() == saved_position);
    }
}

TEST_CASE("the Process List spawns centred and is never remembered", "[window_geometry]")
{
    application();

    const QString    path   = scratch_settings_file("process_list_geometry.ini");
    const QByteArray seeded = seed_window_geometry(path, "windows/process_list_geometry");

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::ui::SettingsController  settings {path};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    QAction* open_process = window.menuBar()->actions().first()->menu()->actions().first();
    REQUIRE(open_process->text() == QStringLiteral("Open Process"));
    open_process->trigger();
    auto* picker = window.findChild<slopkit::ui::dialogs::ProcessListDialog*>();
    REQUIRE(picker != nullptr);
    QCoreApplication::processEvents();

    // The picker opens centred on the main window's frame, not at the seeded
    // position, and keeps its locked 600x440 size.
    CHECK(picker->pos() == centred_on(window, *picker));
    QWidget probe;
    REQUIRE(probe.restoreGeometry(seeded));
    CHECK(picker->pos() != probe.pos());
    CHECK(picker->size() == QSize(600, 440));
    CHECK(picker->minimumSize() == picker->maximumSize());

    picker->move(120, 80);
    QCoreApplication::processEvents();
    picker->hide();
    QCoreApplication::processEvents();

    // No keeper writes a fresh frame: the seeded value is untouched and the
    // settings store holds no geometry for the picker.
    CHECK(settings.values().window_geometry.empty());
    const std::string text = read_file(path);
    CHECK(count_occurrences(text, "process_list_geometry=") == 1);
    QSettings reader {path, QSettings::IniFormat};
    CHECK(reader.value(QStringLiteral("windows/process_list_geometry")).toByteArray() == seeded);
}

TEST_CASE("Add Address, Settings and Table Settings spawn centred and are not remembered", "[window_geometry]")
{
    application();

    const QString    path            = scratch_settings_file("three_dialogs_geometry.ini");
    const QByteArray seeded_add      = seed_window_geometry(path, "windows/add_address_geometry");
    const QByteArray seeded_table    = seed_window_geometry(path, "windows/table_settings_geometry");
    const QByteArray seeded_settings = seed_window_geometry(path, "windows/settings_geometry");

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::ui::SettingsController  settings {path};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};
    // The offscreen screen is 800x800; size the main window so every centred
    // dialog lands fully on-screen and the platform leaves the position alone.
    window.resize(780, 780);

    // Add Address, through the address list's table menu.
    trigger_table_command(window, QStringLiteral("Add Address Manually…"));
    auto* add_dialog = window.findChild<slopkit::ui::dialogs::AddAddressDialog*>();
    REQUIRE(add_dialog != nullptr);
    QCoreApplication::processEvents();
    CHECK(add_dialog->isVisible());
    CHECK(add_dialog->pos() == centred_on(window, *add_dialog));
    add_dialog->hide();
    QCoreApplication::processEvents();

    // Table Settings, through the address list's table menu.
    trigger_table_command(window, QStringLiteral("Table Settings…"));
    auto* table_dialog = window.findChild<slopkit::ui::dialogs::TableSettingsDialog*>();
    REQUIRE(table_dialog != nullptr);
    QCoreApplication::processEvents();
    CHECK(table_dialog->isVisible());
    CHECK(table_dialog->pos() == centred_on(window, *table_dialog));
    table_dialog->hide();
    QCoreApplication::processEvents();

    // Settings, through the View menu.
    QAction* settings_action = nullptr;
    for (QAction* action : window.menuBar()->actions().at(1)->menu()->actions())
    {
        if (action->text() == QStringLiteral("Settings"))
        {
            settings_action = action;
        }
    }
    REQUIRE(settings_action != nullptr);
    settings_action->trigger();
    auto* settings_dialog = window.findChild<slopkit::ui::dialogs::SettingsDialog*>();
    REQUIRE(settings_dialog != nullptr);
    QCoreApplication::processEvents();
    CHECK(settings_dialog->isVisible());
    CHECK(settings_dialog->pos() == centred_on(window, *settings_dialog));
    settings_dialog->hide();
    QCoreApplication::processEvents();

    // None of the seeded frames was applied or rewritten.
    CHECK(settings.values().window_geometry.empty());
    const std::string text = read_file(path);
    CHECK(count_occurrences(text, "add_address_geometry=") == 1);
    CHECK(count_occurrences(text, "table_settings_geometry=") == 1);
    // Anchored so `settings_geometry=` is not matched inside `table_settings_geometry=`.
    CHECK(count_occurrences(text, "\nsettings_geometry=") == 1);
    QSettings reader {path, QSettings::IniFormat};
    CHECK(reader.value(QStringLiteral("windows/add_address_geometry")).toByteArray() == seeded_add);
    CHECK(reader.value(QStringLiteral("windows/table_settings_geometry")).toByteArray() == seeded_table);
    CHECK(reader.value(QStringLiteral("windows/settings_geometry")).toByteArray() == seeded_settings);
}

TEST_CASE("input and message boxes are never remembered", "[window_geometry]")
{
    application();

    const QString                   path = scratch_settings_file("boxes_geometry.ini");
    slopkit::ui::SettingsController settings {path};
    CHECK(settings.values().window_geometry.empty());

    slopkit::ui::widgets::InputBoxOptions options;
    options.title = QStringLiteral("Title");
    options.label = QStringLiteral("Label");
    slopkit::ui::widgets::InputBox input {options};
    input.show();
    QCoreApplication::processEvents();
    input.hide();
    QCoreApplication::processEvents();

    slopkit::ui::widgets::MessageBox box;
    box.set_text(QStringLiteral("Text"));
    box.show();
    QCoreApplication::processEvents();
    box.hide();
    QCoreApplication::processEvents();

    slopkit::ui::dialogs::TableConflictInfo   info;
    slopkit::ui::dialogs::TableConflictDialog conflict {info};
    conflict.show();
    QCoreApplication::processEvents();
    conflict.hide();
    QCoreApplication::processEvents();

    // The transient prompts never touch the settings store: no geometry entry.
    CHECK(settings.values().window_geometry.empty());
}
