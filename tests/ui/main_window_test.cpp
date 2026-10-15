#include <catch2/catch.hpp>

#include <fstream>
#include <string>
#include <vector>

#include "support/ui_helpers.hpp"
#include "ui/dialogs/add_script.hpp"
#include "ui/dialogs/reclass.hpp"
#include "ui/panels/debug_controls.hpp"

TEST_CASE("the main window shell is built", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::ui::SettingsController  settings {scratch_settings_file("shell.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    const QList<QAction*> menus = window.menuBar()->actions();
    REQUIRE(menus.size() == 3);
    CHECK(menus[0]->text() == QStringLiteral("File"));
    CHECK(menus[1]->text() == QStringLiteral("View"));
    CHECK(menus[2]->text() == QStringLiteral("Help"));

    CHECK(action_texts(menus[0]->menu()->actions())
          == QList<QString> {QStringLiteral("Open Process"),
                             QStringLiteral("Open Table"),
                             QStringLiteral("Save Table"),
                             QStringLiteral("Save Table As"),
                             QStringLiteral("Add Script…"),
                             QStringLiteral("Quit")});

    CHECK(window.findChild<QToolBar*>(QStringLiteral("main_toolbar")) == nullptr);

    CHECK(action_texts(menus[1]->menu()->actions())
          == QList<QString> {QStringLiteral("Log"),
                             QStringLiteral("Settings"),
                             QStringLiteral("Breakpoints"),
                             QStringLiteral("Access Watch"),
                             QStringLiteral("ReClass")});
    CHECK(action_texts(menus[2]->menu()->actions())
          == QList<QString> {QStringLiteral("Launch Practice Target"), QStringLiteral("About slopkit")});

    for (QAction* menu : window.menuBar()->actions())
    {
        CHECK(menu->text() != QStringLiteral("Edit"));
    }
    for (QAction* action : window.findChildren<QAction*>())
    {
        CHECK(action->text() != QStringLiteral("Undo Scan"));
        CHECK(action->text() != QStringLiteral("Add Address Manually..."));
        CHECK(action->text() != QStringLiteral("Table Settings"));
    }

    const QList<QAction*> file_actions = menus[0]->menu()->actions();
    REQUIRE(file_actions.size() == 7);
    CHECK(file_actions[0]->shortcut() == QKeySequence(QStringLiteral("Ctrl+T")));
    CHECK(file_actions[1]->shortcut() == QKeySequence(QKeySequence::Open));
    CHECK(file_actions[2]->shortcut() == QKeySequence(QKeySequence::Save));
    CHECK(file_actions[3]->shortcut() == QKeySequence(QKeySequence::SaveAs));

    for (const int index : {0, 1, 2, 3, 4})
    {
        CHECK_FALSE(file_actions[index]->icon().isNull());
        CHECK(file_actions[index]->icon().availableSizes().contains(QSize(16, 16)));
    }
    CHECK(file_actions[2]->icon().pixmap(16).toImage() == file_actions[3]->icon().pixmap(16).toImage());
    CHECK(file_actions[5]->isSeparator());
    CHECK(file_actions[6]->icon().isNull());
    for (QAction* action : menus[1]->menu()->actions())
    {
        CHECK(action->icon().isNull());
    }
    for (QAction* action : menus[2]->menu()->actions())
    {
        CHECK(action->icon().isNull());
    }

    slopkit::log::Logger::instance().clear_history();
    slopkit::log::Logger::instance().set_minimum_level(slopkit::log::Level::info);

    QAction* log_action = menus[1]->menu()->actions().first();
    CHECK(log_action->text() == QStringLiteral("Log"));
    log_action->trigger();
    auto* log_dialog = window.log();
    REQUIRE(log_dialog != nullptr);
    CHECK(log_dialog->isVisible());

    auto* log_view = log_dialog->findChild<QPlainTextEdit*>();
    REQUIRE(log_view != nullptr);
    slopkit::log::info("shell", "live record");
    QCoreApplication::processEvents();
    CHECK(log_view->toPlainText().contains(QStringLiteral("[shell] live record")));

    auto* log_search = log_dialog->findChild<QLineEdit*>();
    REQUIRE(log_search != nullptr);
    log_search->setText(QStringLiteral("no-such-record"));
    CHECK(log_view->toPlainText().isEmpty());
    log_search->clear();

    log_action->trigger();
    CHECK(window.log() == log_dialog);
    CHECK(window.findChildren<slopkit::ui::dialogs::LogDialog*>().isEmpty());

    QAction* breakpoints_action = menus[1]->menu()->actions().at(2);
    CHECK(breakpoints_action->text() == QStringLiteral("Breakpoints"));
    breakpoints_action->trigger();
    auto* breakpoints_dialog = window.breakpoints();
    REQUIRE(breakpoints_dialog != nullptr);
    CHECK(breakpoints_dialog->isVisible());
    breakpoints_action->trigger();
    CHECK(window.breakpoints() == breakpoints_dialog);
    CHECK(window.findChildren<slopkit::ui::dialogs::BreakpointsDialog*>().isEmpty());

    QAction* access_watch_action = menus[1]->menu()->actions().at(3);
    CHECK(access_watch_action->text() == QStringLiteral("Access Watch"));
    access_watch_action->trigger();
    auto* access_watch_dialog = window.access_watch();
    REQUIRE(access_watch_dialog != nullptr);
    CHECK(access_watch_dialog->isVisible());
    access_watch_action->trigger();
    CHECK(window.access_watch() == access_watch_dialog);
    CHECK(window.findChildren<slopkit::ui::dialogs::AccessWatchDialog*>().isEmpty());

    QAction* reclass_action = menus[1]->menu()->actions().at(4);
    CHECK(reclass_action->text() == QStringLiteral("ReClass"));
    reclass_action->trigger();
    auto* reclass_dialog = window.reclass();
    REQUIRE(reclass_dialog != nullptr);
    CHECK(reclass_dialog->isVisible());
    reclass_action->trigger();
    CHECK(window.reclass() == reclass_dialog);
    CHECK(window.findChildren<slopkit::ui::dialogs::ReClassDialog*>().isEmpty());

    slopkit::log::Logger::instance().set_minimum_level(slopkit::log::Level::info);

    auto* process_label = window.statusBar()->findChild<QLabel*>(QStringLiteral("process_label"));
    REQUIRE(process_label != nullptr);
    CHECK(process_label->text() == QStringLiteral("No Process Selected"));
    auto* address_status =
        window.statusBar()->findChild<slopkit::ui::widgets::StatusLabel*>(QStringLiteral("address_status"));
    CHECK(address_status != nullptr);
    CHECK(window.statusBar()->findChild<QProgressBar*>() == nullptr);

    auto* column = window.centralWidget();
    REQUIRE(column != nullptr);
    auto* progress = column->findChild<QProgressBar*>();
    REQUIRE(progress != nullptr);
    CHECK(progress->value() == 0);

    QSplitter* vertical = nullptr;
    for (auto* splitter : column->findChildren<QSplitter*>())
    {
        if (splitter->orientation() == Qt::Vertical)
        {
            vertical = splitter;
        }
    }
    REQUIRE(vertical != nullptr);
    REQUIRE(vertical->count() == 2);

    // The vertical splitter's upper child carries the scan zone and, under it,
    // the companion bar that spans the window.
    auto* upper = vertical->widget(0);
    REQUIRE(upper != nullptr);
    auto* middle = upper->findChild<QSplitter*>();
    REQUIRE(middle != nullptr);
    CHECK(middle->orientation() == Qt::Horizontal);
    CHECK(middle->count() == 2);
    auto* companions = upper->findChild<slopkit::ui::panels::CompanionBar*>();
    REQUIRE(companions != nullptr);

    CHECK(window.size() == QSize(748, 768));

    auto* found_list = window.findChild<slopkit::ui::panels::FoundListPanel*>();
    REQUIRE(found_list != nullptr);
    auto* scanner = window.findChild<slopkit::ui::panels::ScannerPanel*>();
    REQUIRE(scanner != nullptr);

    // The scanner options are a plain panel, not a collapsible section: the old
    // CollapsibleSection used a QToolButton toggle, so none must remain.
    bool has_hex_checkbox = false;
    for (auto* box : scanner->findChildren<QCheckBox*>())
    {
        has_hex_checkbox = has_hex_checkbox || box->text() == QStringLiteral("Hex");
    }
    CHECK(has_hex_checkbox);
    CHECK(scanner->findChild<QToolButton*>() == nullptr);

    CHECK(checkbox_labelled(*scanner, QStringLiteral("Lua formula")) == nullptr);
    CHECK(checkbox_labelled(*scanner, QStringLiteral("Enable Speedhack")) == nullptr);
    CHECK(checkbox_labelled(*scanner, QStringLiteral("Not")) == nullptr);
    CHECK(checkbox_labelled(*scanner, QStringLiteral("Unrandomizer")) == nullptr);
    CHECK_FALSE(panel_has_title(scanner, QStringLiteral("Extra options")));

    auto* writable_check = checkbox_labelled(*scanner, QStringLiteral("Writable"));
    REQUIRE(writable_check != nullptr);
    auto* options_panel = ancestor_panel(writable_check);
    REQUIRE(options_panel != nullptr);
    CHECK(panel_has_title(options_panel, QStringLiteral("Memory Scan Options")));

    auto* found_header = found_list->findChild<QLabel*>();
    REQUIRE(found_header != nullptr);
    CHECK(found_header->text() == QStringLiteral("Showing 0 of 0 results"));

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);
    for (auto* button : address_list->findChildren<QPushButton*>())
    {
        CHECK(button->text() != QStringLiteral("Advanced Options"));
        CHECK(button->text() != QStringLiteral("Table Extras"));
    }

    for (auto* label : window.findChildren<QLabel*>())
    {
        CHECK(label->text() != QStringLiteral("Select a process to enable scanning."));
    }

    window.show();
    QCoreApplication::processEvents();

    auto* hits = found_list->findChild<QTableView*>();
    REQUIRE(hits != nullptr);
    const auto hits_bottom    = hits->mapTo(&window, QPoint(0, hits->height()));
    const auto options_bottom = options_panel->mapTo(&window, QPoint(0, options_panel->height()));
    CHECK(hits_bottom.y() == options_bottom.y());

    // The companion bar spans the window, so the found list is not as wide as
    // the buttons under it.
    CHECK(companions->width() > found_list->width());

    const int scan_zone_before = middle->height();
    const int address_before   = address_list->height();
    window.resize(window.width(), window.height() + 200);
    QCoreApplication::processEvents();
    CHECK(middle->height() == scan_zone_before);
    CHECK(address_list->height() - address_before >= 150);
    CHECK(hits->mapTo(&window, QPoint(0, hits->height())).y()
          == options_panel->mapTo(&window, QPoint(0, options_panel->height())).y());

    // The removed scanner buttons are gone from the whole window; the two
    // commands now live in the address list's context menu instead.
    CHECK(button_labelled(window, QStringLiteral("Add Address Manually")) == nullptr);
    CHECK(button_labelled(window, QStringLiteral("Table Settings")) == nullptr);

    slopkit::ui::apply_theme(slopkit::ui::light_theme());
    QCoreApplication::processEvents();
    CHECK(QGuiApplication::palette().color(QPalette::Window) == slopkit::ui::light_theme().background);
}

TEST_CASE("the main window's tab order follows the scan flow", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::ui::SettingsController  settings {scratch_settings_file("tab_order.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    auto* scanner = window.findChild<slopkit::ui::panels::ScannerPanel*>();
    REQUIRE(scanner != nullptr);
    auto* found_list = window.findChild<slopkit::ui::panels::FoundListPanel*>();
    REQUIRE(found_list != nullptr);
    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);
    auto* companions = window.findChild<slopkit::ui::panels::CompanionBar*>();
    REQUIRE(companions != nullptr);

    auto combo_showing = [](QWidget& root, const QString& text) -> QComboBox*
    {
        for (auto* combo : root.findChildren<QComboBox*>())
        {
            if (combo->currentText() == text)
            {
                return combo;
            }
        }
        return nullptr;
    };

    auto* first_scan      = button_labelled(*scanner, QStringLiteral("First Scan"));
    auto* next_scan       = button_labelled(*scanner, QStringLiteral("Next Scan"));
    auto* undo_scan       = button_labelled(*scanner, QStringLiteral("Undo Scan"));
    auto* cancel          = button_labelled(*scanner, QStringLiteral("Cancel"));
    auto* hex             = checkbox_labelled(*scanner, QStringLiteral("Hex"));
    auto* value           = address_field(*scanner, "Value");
    auto* upper_value     = address_field(*scanner, "Upper value");
    auto* scan_type       = combo_showing(*scanner, QStringLiteral("Exact Value"));
    auto* value_type      = combo_showing(*scanner, QStringLiteral("4 Bytes"));
    auto* memory_region   = range_combo(*scanner);
    auto* start_address   = address_field(*scanner, "Start address");
    auto* stop_address    = address_field(*scanner, "Stop address");
    auto* writable        = checkbox_labelled(*scanner, QStringLiteral("Writable"));
    auto* executable      = checkbox_labelled(*scanner, QStringLiteral("Executable"));
    auto* copy_on_write   = checkbox_labelled(*scanner, QStringLiteral("CopyOnWrite"));
    auto* fast_scan       = checkbox_labelled(*scanner, QStringLiteral("Fast Scan"));
    auto* alignment       = address_field(*scanner, "Alignment");
    auto* pause           = checkbox_labelled(*scanner, QStringLiteral("Pause the game while scanning"));
    auto* hits            = found_list->findChild<QTableView*>();
    auto* memory_view     = button_labelled(*companions, QStringLiteral("Memory View"));
    auto* reclass         = button_labelled(*companions, QStringLiteral("ReClass"));
    auto* breakpoints     = button_labelled(*companions, QStringLiteral("Breakpoints"));
    auto* access_watch    = button_labelled(*companions, QStringLiteral("Access Watch"));
    auto* log_button      = button_labelled(*companions, QStringLiteral("Log"));
    auto* settings_button = button_labelled(*companions, QStringLiteral("Settings"));
    auto* addresses       = address_list->findChild<QTableView*>();

    const std::vector<QWidget*> order {
        first_scan,    next_scan,    undo_scan,     cancel,          hex,          value,       upper_value,
        scan_type,     value_type,   memory_region, start_address,   stop_address, writable,    executable,
        copy_on_write, fast_scan,    alignment,     pause,           hits,         memory_view, reclass,
        breakpoints,   access_watch, log_button,    settings_button, addresses};
    for (QWidget* widget : order)
    {
        REQUIRE(widget != nullptr);
    }

    for (std::size_t index = 0; index + 1 < order.size(); ++index)
    {
        check_tab_order(window, order[index], order[index + 1]);
    }

    CHECK(cancel->isHidden());
    CHECK(upper_value->isHidden());

    QWidget* head = nullptr;
    for (QWidget* widget = window.nextInFocusChain(); widget != &window; widget = widget->nextInFocusChain())
    {
        if (widget->focusPolicy() != Qt::NoFocus)
        {
            head = widget;
            break;
        }
    }
    CHECK(head == first_scan);
}

TEST_CASE("re-activating the main window focuses the scanner value box and selects its text", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::ui::SettingsController  settings {scratch_settings_file("window_activate.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    auto* scanner = window.findChild<slopkit::ui::panels::ScannerPanel*>();
    REQUIRE(scanner != nullptr);
    auto* value = address_field(*scanner, "Value");
    REQUIRE(value != nullptr);
    value->setText(QStringLiteral("4242"));

    window.show();

    QEvent activate {QEvent::WindowActivate};
    QCoreApplication::sendEvent(&window, &activate);

    CHECK(window.focusWidget() == value);
    CHECK(value->selectedText() == QStringLiteral("4242"));
}

TEST_CASE("the memory viewer is a detached top-level window", "[ui]")
{
    application();

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::ui::SettingsController  settings {scratch_settings_file("detached_viewer.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    auto* viewer = window.memory_viewer();
    REQUIRE(viewer != nullptr);
    // No parent widget and not a child of the window: the compositor is free to
    // stack it normally instead of keeping it above the main window.
    CHECK(viewer->parentWidget() == nullptr);
    CHECK(viewer->isWindow());
    CHECK_FALSE(window.isAncestorOf(viewer));
    CHECK(window.findChild<slopkit::ui::dialogs::MemoryViewerDialog*>() == nullptr);
    // A companion window, not a window that owns the process lifetime: closing
    // the shell must still end slopkit.
    CHECK_FALSE(viewer->testAttribute(Qt::WA_QuitOnClose));
}

TEST_CASE("the companion windows are detached top-level windows", "[ui]")
{
    application();

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::ui::SettingsController  settings {scratch_settings_file("detached_log.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    // No parent widget and not a child of the window: the compositor is free to
    // stack each companion normally instead of keeping it above the shell.
    auto* log_window = window.log();
    REQUIRE(log_window != nullptr);
    CHECK(log_window->parentWidget() == nullptr);
    CHECK(log_window->isWindow());
    CHECK_FALSE(window.isAncestorOf(log_window));
    CHECK(window.findChild<slopkit::ui::dialogs::LogDialog*>() == nullptr);
    // A companion window, not a window that owns the process lifetime: closing
    // the shell must still end slopkit.
    CHECK_FALSE(log_window->testAttribute(Qt::WA_QuitOnClose));

    auto* breakpoints_window = window.breakpoints();
    REQUIRE(breakpoints_window != nullptr);
    CHECK(breakpoints_window->parentWidget() == nullptr);
    CHECK(breakpoints_window->isWindow());
    CHECK_FALSE(window.isAncestorOf(breakpoints_window));
    CHECK(window.findChild<slopkit::ui::dialogs::BreakpointsDialog*>() == nullptr);
    CHECK_FALSE(breakpoints_window->testAttribute(Qt::WA_QuitOnClose));

    auto* access_watch_window = window.access_watch();
    REQUIRE(access_watch_window != nullptr);
    CHECK(access_watch_window->parentWidget() == nullptr);
    CHECK(access_watch_window->isWindow());
    CHECK_FALSE(window.isAncestorOf(access_watch_window));
    CHECK(window.findChild<slopkit::ui::dialogs::AccessWatchDialog*>() == nullptr);
    CHECK_FALSE(access_watch_window->testAttribute(Qt::WA_QuitOnClose));
}

TEST_CASE("the debug control bar's Breakpoints button opens the window", "[ui]")
{
    application();

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::ui::SettingsController  settings {scratch_settings_file("debug_controls_breakpoints.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    window.show();
    auto* viewer = window.memory_viewer();
    REQUIRE(viewer != nullptr);
    auto* controls = viewer->findChild<slopkit::ui::panels::DebugControls*>();
    REQUIRE(controls != nullptr);

    controls->breakpoints_button()->click();
    auto* dialog = window.breakpoints();
    REQUIRE(dialog != nullptr);
    CHECK(dialog->isVisible());
}

TEST_CASE("the companion bar's buttons open their windows", "[ui]")
{
    application();

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::ui::SettingsController  settings {scratch_settings_file("companion_bar.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    window.show();
    QCoreApplication::processEvents();

    auto* companions = window.findChild<slopkit::ui::panels::CompanionBar*>();
    REQUIRE(companions != nullptr);

    auto* settings_dialog = window.findChild<slopkit::ui::dialogs::SettingsDialog*>();
    REQUIRE(settings_dialog != nullptr);

    const std::pair<QString, QWidget*> dialogs[] = {
        {         QStringLiteral("Log"),          window.log()},
        {    QStringLiteral("Settings"),       settings_dialog},
        { QStringLiteral("Breakpoints"),  window.breakpoints()},
        {QStringLiteral("Access Watch"), window.access_watch()},
        {     QStringLiteral("ReClass"),      window.reclass()}
    };

    for (const auto& [label, dialog] : dialogs)
    {
        REQUIRE(dialog != nullptr);
        auto* button = button_labelled(*companions, label);
        REQUIRE(button != nullptr);
        // The bar spans the window, so every label stays whole at the default
        // size.
        CHECK(button->width() >= button->sizeHint().width());
        button->click();
        CHECK(dialog->isVisible());
        // A second click re-shows the same instance instead of creating another.
        button->click();
        CHECK(dialog->isVisible());
    }

    CHECK(window.findChildren<slopkit::ui::dialogs::LogDialog*>().isEmpty());
    CHECK(window.findChildren<slopkit::ui::dialogs::BreakpointsDialog*>().isEmpty());
    CHECK(window.findChildren<slopkit::ui::dialogs::AccessWatchDialog*>().isEmpty());
    CHECK(window.findChildren<slopkit::ui::dialogs::ReClassDialog*>().isEmpty());
    CHECK(window.findChildren<slopkit::ui::dialogs::SettingsDialog*>().size() == 1);
}

TEST_CASE("closing the main window ends the session", "[ui]")
{
    application();

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::ui::SettingsController  settings {scratch_settings_file("shell_close.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    window.show();
    auto* viewer = window.memory_viewer();
    REQUIRE(viewer != nullptr);
    viewer->show();
    QCoreApplication::processEvents();
    REQUIRE(window.isVisible());
    REQUIRE(viewer->isVisible());

    window.close();
    QCoreApplication::processEvents();

    CHECK_FALSE(window.isVisible());
    // Qt's last-window-closed handling ends the event loop once no visible
    // top-level window owns the process lifetime; the shell alone owns it.
    int lifetime_owners = 0;
    for (QWidget* widget : QApplication::topLevelWidgets())
    {
        if (widget->isVisible() && widget->testAttribute(Qt::WA_QuitOnClose))
        {
            ++lifetime_owners;
        }
    }
    CHECK(lifetime_owners == 0);
}

TEST_CASE("the main window remembers its geometry across a restart", "[ui]")
{
    application();

    const QString path = scratch_settings_file("main_geometry.ini");
    QSize         saved_size;
    QPoint        saved_position;
    {
        slopkit::plugin::PluginHost      host;
        slopkit::process::PluginAccess   access {host};
        slopkit::process::AccessWorker   worker {access};
        slopkit::process::AttachedTarget target;
        slopkit::ui::SettingsController  settings {path};
        slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

        window.resize(700, 700);
        window.show();
        QCoreApplication::processEvents();
        window.move(25, 35);
        QCoreApplication::processEvents();
        saved_size     = window.size();
        saved_position = window.pos();
        CHECK(saved_size != QSize(748, 768));
        window.hide();
        QCoreApplication::processEvents();

        CHECK_FALSE(settings.window_geometry(slopkit::ui::WindowId::main).isEmpty());
    }

    {
        slopkit::plugin::PluginHost      host;
        slopkit::process::PluginAccess   access {host};
        slopkit::process::AccessWorker   worker {access};
        slopkit::process::AttachedTarget target;
        slopkit::ui::SettingsController  settings {path};
        slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

        window.show();
        QCoreApplication::processEvents();

        CHECK(window.size() == saved_size);
        CHECK(window.pos() == saved_position);
    }
}

TEST_CASE("a corrupt main-window geometry leaves the default size", "[ui]")
{
    application();

    const QString path = scratch_settings_file("main_geometry_junk.ini");
    {
        std::ofstream file(path.toStdString(), std::ios::binary | std::ios::trunc);
        file << "[windows]\nmain_geometry=not-a-blob\n";
    }

    std::vector<slopkit::log::Record> records;
    SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                            {
                        records.push_back(record);
                                            }};

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::ui::SettingsController  settings {path};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    CHECK(window.size() == QSize(748, 768));

    int warnings = 0;
    for (const slopkit::log::Record& record : records)
    {
        if (record.level == slopkit::log::Level::warning
            && record.message.find("malformed windows/main_geometry value") != std::string::npos)
        {
            ++warnings;
        }
    }
    CHECK(warnings == 1);
}

TEST_CASE("File > Quit closes the window and stores the geometry", "[ui]")
{
    application();

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::ui::SettingsController  settings {scratch_settings_file("shell_quit.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    window.show();
    auto* viewer = window.memory_viewer();
    REQUIRE(viewer != nullptr);
    viewer->show();
    QCoreApplication::processEvents();
    REQUIRE(window.isVisible());

    QAction* quit = nullptr;
    for (QAction* action : window.menuBar()->actions().first()->menu()->actions())
    {
        if (action->text() == QStringLiteral("Quit"))
        {
            quit = action;
        }
    }
    REQUIRE(quit != nullptr);
    quit->trigger();
    QCoreApplication::processEvents();

    // Quit goes through the same close path as the window-manager button, so the
    // geometry keeper writes the frame and no visible top-level still owns the
    // process lifetime.
    CHECK_FALSE(window.isVisible());
    CHECK_FALSE(settings.window_geometry(slopkit::ui::WindowId::main).isEmpty());

    int lifetime_owners = 0;
    for (QWidget* widget : QApplication::topLevelWidgets())
    {
        if (widget->isVisible() && widget->testAttribute(Qt::WA_QuitOnClose))
        {
            ++lifetime_owners;
        }
    }
    CHECK(lifetime_owners == 0);
}

TEST_CASE("File > Add Script opens the dialog and appends a script row", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::ui::SettingsController  settings {scratch_settings_file("add_script.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    QAction* add_script = nullptr;
    for (QAction* action : window.menuBar()->actions().first()->menu()->actions())
    {
        if (action->text() == QStringLiteral("Add Script…"))
        {
            add_script = action;
        }
    }
    REQUIRE(add_script != nullptr);

    add_script->trigger();
    QCoreApplication::processEvents();

    auto* dialog = window.findChild<slopkit::ui::dialogs::AddScriptDialog*>();
    REQUIRE(dialog != nullptr);
    CHECK(dialog->isVisible());

    auto* description = dialog->findChild<QLineEdit*>(QStringLiteral("description_edit"));
    auto* editor      = dialog->findChild<QPlainTextEdit*>(QStringLiteral("script_edit"));
    auto* commit      = dialog->findChild<QPushButton*>(QStringLiteral("commit_button"));
    REQUIRE(description != nullptr);
    REQUIRE(editor != nullptr);
    REQUIRE(commit != nullptr);

    description->clear();
    commit->click();
    CHECK(dialog->isVisible());

    description->setText(QStringLiteral("helper"));
    editor->setPlainText(QStringLiteral("print('hi')\nreturn 1"));
    commit->click();
    QCoreApplication::processEvents();

    CHECK_FALSE(dialog->isVisible());

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);
    auto* view = address_list->findChild<QTableView*>();
    REQUIRE(view != nullptr);
    REQUIRE(view->model() != nullptr);
    REQUIRE(view->model()->rowCount() == 1);
    CHECK(view->model()->index(0, 1).data(Qt::DisplayRole).toString() == QStringLiteral("helper"));
    CHECK(view->model()->index(0, 2).data(Qt::DisplayRole).toString().isEmpty());
    CHECK(view->model()->index(0, 3).data(Qt::DisplayRole).toString() == QStringLiteral("script"));
    CHECK(view->model()->index(0, 4).data(Qt::DisplayRole).toString().isEmpty());
}

TEST_CASE("the Hook Instruction relay opens the address list's Add Script window", "[ui]")
{
    application();

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::ui::SettingsController  settings {scratch_settings_file("hook_relay.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    auto* viewer = window.memory_viewer();
    REQUIRE(viewer != nullptr);

    const QString description = QStringLiteral("Hook app+1A2B40");
    const QString source      = QStringLiteral("function activate()\n    return true\nend\n");
    REQUIRE(QMetaObject::invokeMethod(
        viewer, "hookScriptRequested", Qt::DirectConnection, Q_ARG(QString, description), Q_ARG(QString, source)));

    auto* dialog = window.findChild<slopkit::ui::dialogs::AddScriptDialog*>();
    REQUIRE(dialog != nullptr);
    auto* description_edit = dialog->findChild<QLineEdit*>(QStringLiteral("description_edit"));
    auto* script_edit      = dialog->findChild<QPlainTextEdit*>(QStringLiteral("script_edit"));
    REQUIRE(description_edit != nullptr);
    REQUIRE(script_edit != nullptr);
    CHECK(description_edit->text() == description);
    CHECK(script_edit->toPlainText() == source);

    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);
    auto* view = address_list->findChild<QTableView*>();
    REQUIRE(view != nullptr);
    REQUIRE(view->model() != nullptr);
    CHECK(view->model()->rowCount() == 0);
}
