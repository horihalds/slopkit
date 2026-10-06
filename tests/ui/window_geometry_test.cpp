#include <catch2/catch.hpp>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <QCoreApplication>
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

TEST_CASE("the Process List keeps its locked size across a restart", "[window_geometry]")
{
    application();

    const QString path = scratch_settings_file("process_list_geometry.ini");
    QPoint        saved_position;
    {
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
        CHECK(picker->size() == QSize(600, 440));

        picker->move(120, 80);
        QCoreApplication::processEvents();
        saved_position = picker->pos();
        picker->hide();
        QCoreApplication::processEvents();

        // The locked size makes the stored size part inert; the position is what
        // the keeper restores.
        CHECK_FALSE(settings.window_geometry(WindowId::process_list).isEmpty());
    }

    {
        slopkit::plugin::PluginHost      host;
        slopkit::process::PluginAccess   access {host};
        slopkit::process::AccessWorker   worker {access};
        slopkit::process::AttachedTarget target;
        slopkit::ui::SettingsController  settings {path};
        slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

        QAction* open_process = window.menuBar()->actions().first()->menu()->actions().first();
        open_process->trigger();
        auto* picker = window.findChild<slopkit::ui::dialogs::ProcessListDialog*>();
        REQUIRE(picker != nullptr);
        CHECK(picker->size() == QSize(600, 440));
        CHECK(picker->pos() == saved_position);
        CHECK(picker->minimumSize() == picker->maximumSize());
    }
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
