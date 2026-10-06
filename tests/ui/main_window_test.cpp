#include <catch2/catch.hpp>

#include <fstream>
#include <string>
#include <vector>

#include "support/ui_helpers.hpp"
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

    // The menu bar holds exactly File, View and Help; the Edit menu is gone.
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
                             QStringLiteral("Quit")});

    // The toolbar is gone; the file commands live only in the menus now.
    CHECK(window.findChild<QToolBar*>(QStringLiteral("main_toolbar")) == nullptr);

    // The View menu holds the log, settings and the two companion windows; Help
    // offers the practice target ahead of About.
    CHECK(action_texts(menus[1]->menu()->actions())
          == QList<QString> {QStringLiteral("Log"),
                             QStringLiteral("Settings"),
                             QStringLiteral("Breakpoints"),
                             QStringLiteral("Access Watch")});
    CHECK(action_texts(menus[2]->menu()->actions())
          == QList<QString> {QStringLiteral("Launch Practice Target"), QStringLiteral("About slopkit")});

    // No Edit menu survives, and Undo Scan / Add Address Manually... are gone as
    // menu actions anywhere in the window.
    for (QAction* menu : window.menuBar()->actions())
    {
        CHECK(menu->text() != QStringLiteral("Edit"));
    }
    for (QAction* action : window.findChildren<QAction*>())
    {
        CHECK(action->text() != QStringLiteral("Undo Scan"));
        CHECK(action->text() != QStringLiteral("Add Address Manually..."));
    }

    // Ctrl+T / Ctrl+O / Ctrl+S / Ctrl+Shift+S are bound to the file commands.
    const QList<QAction*> file_actions = menus[0]->menu()->actions();
    REQUIRE(file_actions.size() == 6);
    CHECK(file_actions[0]->shortcut() == QKeySequence(QStringLiteral("Ctrl+T")));
    CHECK(file_actions[1]->shortcut() == QKeySequence(QKeySequence::Open));
    CHECK(file_actions[2]->shortcut() == QKeySequence(QKeySequence::Save));
    CHECK(file_actions[3]->shortcut() == QKeySequence(QKeySequence::SaveAs));

    // The four file commands carry their action glyphs; every other menu entry
    // stays text-only.
    for (const int index : {0, 1, 2, 3})
    {
        CHECK_FALSE(file_actions[index]->icon().isNull());
        CHECK(file_actions[index]->icon().availableSizes().contains(QSize(16, 16)));
    }
    // Save and Save As share the same diskette glyph.
    CHECK(file_actions[2]->icon().pixmap(16).toImage() == file_actions[3]->icon().pixmap(16).toImage());
    // Quit, and every View/Help entry, remain icon-less.
    CHECK(file_actions[5]->icon().isNull());
    for (QAction* action : menus[1]->menu()->actions())
    {
        CHECK(action->icon().isNull());
    }
    for (QAction* action : menus[2]->menu()->actions())
    {
        CHECK(action->icon().isNull());
    }

    // View > Log opens the non-modal log window; live records reach its view
    // and the text filter hides what does not match.
    slopkit::log::Logger::instance().clear_history();
    slopkit::log::Logger::instance().set_minimum_level(slopkit::log::Level::info);

    QAction* log_action = menus[1]->menu()->actions().first();
    CHECK(log_action->text() == QStringLiteral("Log"));
    log_action->trigger();
    auto* log_dialog = window.findChild<slopkit::ui::dialogs::LogDialog*>();
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

    // Triggering the entry again raises the same window, not a second one.
    log_action->trigger();
    CHECK(window.findChildren<slopkit::ui::dialogs::LogDialog*>().size() == 1);

    // View > Breakpoints opens the non-modal breakpoints window, once.
    QAction* breakpoints_action = menus[1]->menu()->actions().at(2);
    CHECK(breakpoints_action->text() == QStringLiteral("Breakpoints"));
    breakpoints_action->trigger();
    auto* breakpoints_dialog = window.findChild<slopkit::ui::dialogs::BreakpointsDialog*>();
    REQUIRE(breakpoints_dialog != nullptr);
    CHECK(breakpoints_dialog->isVisible());
    breakpoints_action->trigger();
    CHECK(window.findChildren<slopkit::ui::dialogs::BreakpointsDialog*>().size() == 1);

    // View > Access Watch opens the non-modal watch window, once.
    QAction* access_watch_action = menus[1]->menu()->actions().at(3);
    CHECK(access_watch_action->text() == QStringLiteral("Access Watch"));
    access_watch_action->trigger();
    auto* access_watch_dialog = window.findChild<slopkit::ui::dialogs::AccessWatchDialog*>();
    REQUIRE(access_watch_dialog != nullptr);
    CHECK(access_watch_dialog->isVisible());
    access_watch_action->trigger();
    CHECK(window.findChildren<slopkit::ui::dialogs::AccessWatchDialog*>().size() == 1);

    slopkit::log::Logger::instance().set_minimum_level(slopkit::log::Level::info);

    // The status bar carries the detached-process label in its left slot and the
    // address list's status line in its permanent right slot; no progress bar
    // lives there any more.
    auto* process_label = window.statusBar()->findChild<QLabel*>(QStringLiteral("process_label"));
    REQUIRE(process_label != nullptr);
    CHECK(process_label->text() == QStringLiteral("No Process Selected"));
    auto* address_status =
        window.statusBar()->findChild<slopkit::ui::widgets::StatusLabel*>(QStringLiteral("address_status"));
    CHECK(address_status != nullptr);
    CHECK(window.statusBar()->findChild<QProgressBar*>() == nullptr);

    // The central widget is a column: the single full-width progress bar sits
    // above the split zones.
    auto* column = window.centralWidget();
    REQUIRE(column != nullptr);
    auto* progress = column->findChild<QProgressBar*>();
    REQUIRE(progress != nullptr);
    CHECK(progress->value() == 0);

    // Two split zones inside the middle zone, the address list below them.
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

    auto* middle = qobject_cast<QSplitter*>(vertical->widget(0));
    REQUIRE(middle != nullptr);
    CHECK(middle->orientation() == Qt::Horizontal);
    CHECK(middle->count() == 2);

    // The window opens at the compact default size.
    CHECK(window.size() == QSize(748, 768));

    // The middle zone holds the live found list and the scanner controls.
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

    // The decorative "advanced" checkboxes are gone, along with the whole
    // "Extra options" section and its placeholder options.
    CHECK(checkbox_labelled(*scanner, QStringLiteral("Lua formula")) == nullptr);
    CHECK(checkbox_labelled(*scanner, QStringLiteral("Enable Speedhack")) == nullptr);
    CHECK(checkbox_labelled(*scanner, QStringLiteral("Not")) == nullptr);
    CHECK(checkbox_labelled(*scanner, QStringLiteral("Unrandomizer")) == nullptr);
    CHECK_FALSE(panel_has_title(scanner, QStringLiteral("Extra options")));

    // The surviving options live inside the Memory Scan Options panel.
    auto* writable_check = checkbox_labelled(*scanner, QStringLiteral("Writable"));
    REQUIRE(writable_check != nullptr);
    auto* options_panel = ancestor_panel(writable_check);
    REQUIRE(options_panel != nullptr);
    CHECK(panel_has_title(options_panel, QStringLiteral("Memory Scan Options")));

    auto* found_header = found_list->findChild<QLabel*>();
    REQUIRE(found_header != nullptr);
    CHECK(found_header->text() == QStringLiteral("Showing 0 of 0 results"));

    // The address list lost its decorative footer buttons.
    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);
    for (auto* button : address_list->findChildren<QPushButton*>())
    {
        CHECK(button->text() != QStringLiteral("Advanced Options"));
        CHECK(button->text() != QStringLiteral("Table Extras"));
    }

    // The removed idle hint is nowhere in the window, in any state.
    for (auto* label : window.findChildren<QLabel*>())
    {
        CHECK(label->text() != QStringLiteral("Select a process to enable scanning."));
    }

    // Show the window so the layouts are realised, then check that the hits
    // table's bottom edge is level with the Memory Scan Options panel's.
    window.show();
    QCoreApplication::processEvents();

    auto* hits = found_list->findChild<QTableView*>();
    REQUIRE(hits != nullptr);
    const auto hits_bottom    = hits->mapTo(&window, QPoint(0, hits->height()));
    const auto options_bottom = options_panel->mapTo(&window, QPoint(0, options_panel->height()));
    CHECK(hits_bottom.y() == options_bottom.y());

    // Growing the window hands every extra pixel to the address list: the scan
    // zone keeps its height and the two edges stay level.
    const int scan_zone_before = middle->height();
    const int address_before   = address_list->height();
    window.resize(window.width(), window.height() + 200);
    QCoreApplication::processEvents();
    CHECK(middle->height() == scan_zone_before);
    CHECK(address_list->height() - address_before >= 150);
    CHECK(hits->mapTo(&window, QPoint(0, hits->height())).y()
          == options_panel->mapTo(&window, QPoint(0, options_panel->height())).y());

    // The Add Address button lives at the bottom-right of the scanner panel,
    // not in the found list or the address list.
    CHECK(button_labelled(*address_list, QStringLiteral("Add Address Manually")) == nullptr);
    CHECK(button_labelled(*found_list, QStringLiteral("Add Address Manually")) == nullptr);
    CHECK(button_labelled(*scanner, QStringLiteral("Add Address Manually")) != nullptr);

    // A live theme switch re-installs the application palette.
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

    // The two type combos are found by the value they show on a fresh window.
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

    auto* first_scan     = button_labelled(*scanner, QStringLiteral("First Scan"));
    auto* next_scan      = button_labelled(*scanner, QStringLiteral("Next Scan"));
    auto* undo_scan      = button_labelled(*scanner, QStringLiteral("Undo Scan"));
    auto* cancel         = button_labelled(*scanner, QStringLiteral("Cancel"));
    auto* hex            = checkbox_labelled(*scanner, QStringLiteral("Hex"));
    auto* value          = address_field(*scanner, "Value");
    auto* upper_value    = address_field(*scanner, "Upper value");
    auto* scan_type      = combo_showing(*scanner, QStringLiteral("Exact Value"));
    auto* value_type     = combo_showing(*scanner, QStringLiteral("4 Bytes"));
    auto* memory_region  = range_combo(*scanner);
    auto* start_address  = address_field(*scanner, "Start address");
    auto* stop_address   = address_field(*scanner, "Stop address");
    auto* writable       = checkbox_labelled(*scanner, QStringLiteral("Writable"));
    auto* executable     = checkbox_labelled(*scanner, QStringLiteral("Executable"));
    auto* copy_on_write  = checkbox_labelled(*scanner, QStringLiteral("CopyOnWrite"));
    auto* fast_scan      = checkbox_labelled(*scanner, QStringLiteral("Fast Scan"));
    auto* alignment      = address_field(*scanner, "Alignment");
    auto* pause          = checkbox_labelled(*scanner, QStringLiteral("Pause the game while scanning"));
    auto* hits           = found_list->findChild<QTableView*>();
    auto* memory_view    = button_labelled(*found_list, QStringLiteral("Memory View"));
    auto* add_address    = button_labelled(*scanner, QStringLiteral("Add Address Manually"));
    auto* table_settings = button_labelled(*scanner, QStringLiteral("Table Settings"));
    auto* addresses      = address_list->findChild<QTableView*>();

    const std::vector<QWidget*> order {
        first_scan, next_scan,     undo_scan,     cancel,       hex,         value,          upper_value,   scan_type,
        value_type, memory_region, start_address, stop_address, writable,    executable,     copy_on_write, fast_scan,
        alignment,  pause,         hits,          memory_view,  add_address, table_settings, addresses};
    for (QWidget* widget : order)
    {
        REQUIRE(widget != nullptr);
    }

    // Tab walks the whole sequence in order, so every control is in the chain
    // and nothing has dropped out of it.
    for (std::size_t index = 0; index + 1 < order.size(); ++index)
    {
        check_tab_order(window, order[index], order[index + 1]);
    }

    // The hidden-by-default controls keep their slots: Qt skips a hidden widget
    // while tabbing but leaves it in the chain.
    CHECK(cancel->isHidden());
    CHECK(upper_value->isHidden());

    // The first focusable control of the window's chain is the First Scan button.
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

    // The bar's button reaches the same window the View menu opens.
    controls->breakpoints_button()->click();
    auto* dialog = window.findChild<slopkit::ui::dialogs::BreakpointsDialog*>();
    REQUIRE(dialog != nullptr);
    CHECK(dialog->isVisible());
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
        // The offscreen platform may clamp an over-wide request, so the size to
        // restore is whatever the window actually holds when it is hidden.
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

        // A real restart shows the window again, which is when the restored
        // frame lands on the screen.
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
