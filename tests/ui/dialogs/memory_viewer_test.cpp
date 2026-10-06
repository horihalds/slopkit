#include <catch2/catch.hpp>

#include <cmath>

#include <QWindow>

#include <QPushButton>

#include "support/ui_helpers.hpp"
#include "ui/components/disassembly_view.hpp"
#include "ui/models/register_model.hpp"
#include "ui/panels/debug_controls.hpp"

TEST_CASE("the found-results entry row drives the viewer", "[ui]")
{
    application();

    slopkit::scan::ScanEngine           engine;
    slopkit::table::AddressTable        table;
    slopkit::ui::panels::FoundListPanel panel {engine, table};

    auto* memory_view = button_labelled(panel, QStringLiteral("Memory View"));
    REQUIRE(memory_view != nullptr);
    // The Add Address button moved to the scanner panel's bottom-right.
    CHECK(button_labelled(panel, QStringLiteral("Add Address Manually")) == nullptr);

    // The view button waits for an attached target.
    CHECK_FALSE(memory_view->isEnabled());

    panel.resize(600, 400);
    panel.show();
    QCoreApplication::processEvents();

    // The button sits in the row directly below the hits table.
    auto* hits = panel.findChild<QTableView*>();
    REQUIRE(hits != nullptr);
    CHECK(memory_view->y() > hits->y());

    bool view_requested = false;
    QObject::connect(&panel,
                     &slopkit::ui::panels::FoundListPanel::memoryViewRequested,
                     &panel,
                     [&]
                     {
                         view_requested = true;
                     });

    panel.set_target_attached(true);
    CHECK(memory_view->isEnabled());
    memory_view->click();
    CHECK(view_requested);

    panel.set_target_attached(false);
    CHECK_FALSE(memory_view->isEnabled());
}

TEST_CASE("the found-list entry row opens the viewer at the main module entry", "[ui]")
{
    application();

    FakeAccess access;

    slopkit::process::ModuleInfo image;
    image.base     = 0x1000;
    image.size     = 0x800;
    image.entry    = 0x1040;
    image.kind     = slopkit::process::ModuleKind::elf;
    image.name     = "low";
    image.path     = "/opt/low";
    access.modules = {image};

    slopkit::plugin::PluginHost      host;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    slopkit::ui::SettingsController  settings {scratch_settings_file("entry_row.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings, shared_debug_controller()};

    attach_app_session(worker);

    auto* found_list = window.findChild<slopkit::ui::panels::FoundListPanel*>();
    REQUIRE(found_list != nullptr);
    auto* scanner = window.findChild<slopkit::ui::panels::ScannerPanel*>();
    REQUIRE(scanner != nullptr);

    auto* memory_view = button_labelled(*found_list, QStringLiteral("Memory View"));
    REQUIRE(memory_view != nullptr);
    auto* add_address = button_labelled(*scanner, QStringLiteral("Add Address Manually"));
    REQUIRE(add_address != nullptr);
    CHECK(scanner->isAncestorOf(add_address));
    CHECK(button_labelled(*found_list, QStringLiteral("Add Address Manually")) == nullptr);

    auto* table_settings = button_labelled(*scanner, QStringLiteral("Table Settings"));
    REQUIRE(table_settings != nullptr);
    CHECK(scanner->isAncestorOf(table_settings));

    // The window enables the view button once the target is attached and the
    // main module's map has landed.
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return memory_view->isEnabled() && scanner->main_module_address() != 0;
                    }));

    window.show();
    QCoreApplication::processEvents();

    // The three buttons share one window-wide row: Memory View on the left, Add
    // Address Manually next and Table Settings against the window's right edge.
    const QPoint memory_view_pos    = memory_view->mapTo(&window, QPoint(0, 0));
    const QPoint add_address_pos    = add_address->mapTo(&window, QPoint(0, 0));
    const QPoint table_settings_pos = table_settings->mapTo(&window, QPoint(0, 0));
    CHECK(memory_view_pos.y() == add_address_pos.y());
    CHECK(add_address_pos.y() == table_settings_pos.y());
    CHECK(memory_view_pos.x() < add_address_pos.x());
    CHECK(add_address_pos.x() < table_settings_pos.x());
    CHECK(window.width() - table_settings->mapTo(&window, table_settings->rect().topRight()).x() <= 24);

    auto* viewer = window.memory_viewer();
    REQUIRE(viewer != nullptr);
    // The viewer is a detached top-level window: no parent widget, so no WM
    // transient hint keeps it stacked above the main window.
    CHECK(viewer->parentWidget() == nullptr);
    CHECK(viewer->isWindow());
    CHECK_FALSE(window.isAncestorOf(viewer));
    auto* view = viewer->findChild<slopkit::ui::components::MemoryView*>();
    REQUIRE(view != nullptr);
    auto* document = viewer->findChild<slopkit::ui::components::MemoryViewDocument*>();
    REQUIRE(document != nullptr);

    memory_view->click();
    REQUIRE(viewer->isVisible());
    REQUIRE(viewer->windowHandle() != nullptr);
    CHECK(viewer->windowHandle()->transientParent() == nullptr);
    // The main module's map is loaded, so the entry renders module-relative and
    // the view lands exactly on the entry address.
    CHECK(document->display_text(0x1040) == QStringLiteral("low+40"));
    CHECK(view->first_byte() == 0x1040);

    // The add button reaches the same non-modal dialog as the menu action.
    auto* add_dialog = window.findChild<slopkit::ui::dialogs::AddAddressDialog*>();
    REQUIRE(add_dialog != nullptr);
    CHECK_FALSE(add_dialog->isVisible());
    add_address->click();
    CHECK(add_dialog->isVisible());

    // The Table Settings button reaches its own non-modal dialog.
    auto* settings_dialog = window.findChild<slopkit::ui::dialogs::TableSettingsDialog*>();
    REQUIRE(settings_dialog != nullptr);
    CHECK_FALSE(settings_dialog->isVisible());
    table_settings->click();
    CHECK(settings_dialog->isVisible());
}

TEST_CASE("the memory viewer follows the live pass", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    slopkit::ui::SettingsController  settings {scratch_settings_file("viewer_live_pass.ini")};

    attach_app_session(worker);

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target, shared_debug_controller()};
    auto*                                    view = viewer.findChild<slopkit::ui::components::MemoryView*>();
    REQUIRE(view != nullptr);

    // The window is three panes now: no loading row remains, but the listing and
    // the placeholder register table sit above the byte view.
    CHECK(viewer.findChild<QLabel*>(QStringLiteral("loading_label")) == nullptr);
    auto* listing = viewer.findChild<slopkit::ui::components::DisassemblyView*>();
    REQUIRE(listing != nullptr);
    auto* register_table = viewer.findChild<QTableView*>(QStringLiteral("register_table"));
    REQUIRE(register_table != nullptr);

    // The target serves reads at the requested block base only, so a helper
    // seeds every block of the window the view just asked for.
    const auto seed_window = [&](std::byte value)
    {
        const std::vector<slopkit::ui::LiveRequest> requests = viewer.next_live_request();
        for (const slopkit::ui::LiveRequest& request : requests)
        {
            (*access.memory)[request.address] = std::vector<std::byte>(request.size, value);
        }
        return requests;
    };

    // Showing the dialog asks for the window; its layout may still grow the view
    // once, so pump the event loop until the geometry is stable before seeding the
    // blocks the live pass will actually request.
    viewer.set_address(0x1000);
    viewer.show();
    for (std::size_t guard = 0; guard < 8; ++guard)
    {
        const std::size_t before = view->visible_rows();
        QCoreApplication::processEvents();
        if (view->visible_rows() == before)
        {
            break;
        }
    }
    const auto requests = seed_window(std::byte {0xAB});
    // The byte view's three blocks come first, the listing's code window last.
    REQUIRE(requests.size() == 4);
    const std::uint64_t extent = requests[1].size;
    CHECK(requests[3].id == slopkit::ui::components::DisassemblyDocument::kIdBase);
    CHECK(requests[3].size == slopkit::ui::components::DisassemblyDocument::kWindowSize);

    slopkit::ui::LiveValues live {worker, target, settings};
    live.add_surface(&viewer);
    QObject::connect(&viewer,
                     &slopkit::ui::dialogs::MemoryViewerDialog::liveRefreshRequested,
                     &live,
                     &slopkit::ui::LiveValues::request_now);

    live.request_now();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return view->cell_text(0x1000) == QStringLiteral("AB");
                    }));
    // No indicator row sits below the byte view, so nothing else changes.

    // A changed byte follows on the next pass.
    const std::uint64_t middle                = requests[1].address;
    (*access.memory)[middle][0x1000 - middle] = std::byte {0xCD};
    live.request_now();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return view->cell_text(0x1000) == QStringLiteral("CD");
                    }));

    // A reading that reports the visible window unreadable paints the placeholder
    // cells, and a cell no reading covers at all stays blank.
    slopkit::ui::LiveReading unreadable;
    unreadable.id       = 1;
    unreadable.readable = false;
    viewer.apply_live_readings(std::vector<slopkit::ui::LiveReading> {unreadable});
    CHECK(view->cell_text(0x1000) == QStringLiteral("??"));

    // A completion whose window moved on while the pass was in flight is dropped.
    const std::uint64_t next_base = middle + 2 * extent;
    viewer.hide();
    viewer.set_address(next_base); // hidden, so no pass is submitted
    const std::vector<slopkit::ui::LiveReading> stale {
        slopkit::ui::LiveReading {.id = 1, .readable = true, .bytes = (*access.memory)[middle]}
    };
    viewer.apply_live_readings(stale);
    CHECK(view->cell_text(next_base).isEmpty());

    // Showing it again loads the new window from the seed.
    for (const std::uint64_t base : {next_base - extent, next_base, next_base + extent})
    {
        (*access.memory)[base] = std::vector<std::byte>(extent, std::byte {0xCD});
    }
    viewer.show();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return view->cell_text(next_base) == QStringLiteral("CD");
                    }));
}

TEST_CASE("the memory viewer keeps the top address when its dialog is resized", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    slopkit::ui::SettingsController  settings {scratch_settings_file("viewer_resize.ini")};

    attach_app_session(worker);

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target, shared_debug_controller()};
    auto*                                    view = viewer.findChild<slopkit::ui::components::MemoryView*>();
    REQUIRE(view != nullptr);

    // The target serves reads at the requested block base only, so a helper
    // seeds every block of the window the view just asked for.
    const auto seed_window = [&](std::byte value)
    {
        for (const slopkit::ui::LiveRequest& request : viewer.next_live_request())
        {
            (*access.memory)[request.address] = std::vector<std::byte>(request.size, value);
        }
    };

    // An address that is not a multiple of the fitted row width.
    viewer.set_address(0x1010);
    viewer.show();
    for (std::size_t guard = 0; guard < 8; ++guard)
    {
        const std::size_t before = view->visible_rows();
        QCoreApplication::processEvents();
        if (view->visible_rows() == before)
        {
            break;
        }
    }

    slopkit::ui::LiveValues live {worker, target, settings};
    live.add_surface(&viewer);
    QObject::connect(&viewer,
                     &slopkit::ui::dialogs::MemoryViewerDialog::liveRefreshRequested,
                     &live,
                     &slopkit::ui::LiveValues::request_now);

    seed_window(std::byte {0xEF});
    live.request_now();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return view->cell_text(0x1010) == QStringLiteral("EF");
                    }));

    const std::uint64_t anchor     = view->first_byte();
    const int           wide_width = view->width();
    REQUIRE(anchor == 0x1010);

    // A narrower dialog re-fits the row width but leaves the anchor alone.
    viewer.resize(viewer.width() - 200, viewer.height());
    QCoreApplication::processEvents();
    REQUIRE(view->width() < wide_width);
    CHECK(view->first_byte() == anchor);

    // A wider one does too, and the cell seeded before the resize still renders
    // its bytes after the next pass.
    viewer.resize(viewer.width() + 400, viewer.height());
    QCoreApplication::processEvents();
    REQUIRE(view->width() > wide_width);
    CHECK(view->first_byte() == anchor);

    seed_window(std::byte {0xEF});
    live.request_now();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return view->cell_text(anchor) == QStringLiteral("EF");
                    }));

    viewer.hide();
}

TEST_CASE("the memory viewer go-to accepts module-relative addresses", "[ui]")
{
    application();

    slopkit::process::AttachedTarget target = fake_target();

    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target, shared_debug_controller()};
    auto*                                    view = viewer.findChild<slopkit::ui::components::MemoryView*>();
    REQUIRE(view != nullptr);

    viewer.set_modules({module_image("app", 0x1000, 0x1000)});

    // A module+RVA (case-insensitively) jumps to exactly the resolved address.
    CHECK(viewer.go_to(QStringLiteral("APP+40")));
    CHECK(view->first_byte() == 0x1040);
    CHECK(viewer.go_to(QStringLiteral("app+40")));
    CHECK(view->first_byte() == 0x1040);

    // A plain absolute address works too, with no rounding to the row grid.
    CHECK(viewer.go_to(QStringLiteral("0x2000")));
    CHECK(view->first_byte() == 0x2000);

    // A bare module name jumps to the module base, case-insensitively.
    CHECK(viewer.go_to(QStringLiteral("APP")));
    CHECK(view->first_byte() == 0x1000);
    CHECK(viewer.go_to(QStringLiteral("app")));
    CHECK(view->first_byte() == 0x1000);

    // An unparsable value leaves the page where it was.
    const std::uint64_t before = view->first_byte();
    CHECK_FALSE(viewer.go_to(QStringLiteral("missing")));
    CHECK(view->first_byte() == before);
    CHECK_FALSE(viewer.go_to(QStringLiteral("missing+40")));
    CHECK(view->first_byte() == before);
}

TEST_CASE("the memory viewer opens Go To with Ctrl+G", "[ui]")
{
    application();

    slopkit::process::AttachedTarget target = fake_target();

    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target, shared_debug_controller()};
    auto*                                    view = viewer.findChild<slopkit::ui::components::MemoryView*>();
    REQUIRE(view != nullptr);
    REQUIRE(view->goto_action() != nullptr);
    CHECK(view->goto_action()->shortcut() == QKeySequence(QStringLiteral("Ctrl+G")));

    viewer.set_address(0x1000);
    viewer.show();
    view->setFocus();
    QCoreApplication::processEvents();

    // Answers the prompt as soon as it opens, recording that it did.
    bool       opened        = false;
    const auto answer_prompt = [&opened]
    {
        QTimer::singleShot(0,
                           [&opened]
                           {
                               for (QWidget* widget : QApplication::topLevelWidgets())
                               {
                                   auto* box = qobject_cast<slopkit::ui::widgets::InputBox*>(widget);
                                   if (box != nullptr && box->isVisible())
                                   {
                                       opened = true;
                                       box->line_edit()->setText(QStringLiteral("0x2000"));
                                       box->accept();
                                       return;
                                   }
                               }
                           });
    };

    // Ctrl+G opens the prompt; the offscreen platform can decline to dispatch
    // window shortcuts, so fall back to the action the shortcut triggers.
    answer_prompt();
    QKeyEvent ctrl_g(QEvent::KeyPress, Qt::Key_G, Qt::ControlModifier);
    QApplication::sendEvent(view, &ctrl_g);
    QCoreApplication::processEvents();
    if (!opened)
    {
        answer_prompt();
        view->goto_action()->trigger();
        QCoreApplication::processEvents();
    }

    CHECK(opened);
    CHECK(view->first_byte() == 0x2000);
    viewer.hide();
}

TEST_CASE("the memory viewer go-to follows a pointer-chain expression", "[ui]")
{
    application();

    slopkit::process::AttachedTarget target = fake_target();

    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};
    attach_app_session(worker);

    // A pointer stored at the module base points the chain onward.
    constexpr std::uint64_t pointer = 0x200000;
    std::vector<std::byte>  bytes(8);
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        bytes[index] = static_cast<std::byte>((pointer >> (8 * index)) & 0xFF);
    }
    (*access.memory)[0x100000] = std::move(bytes);

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target, shared_debug_controller()};
    auto*                                    view = viewer.findChild<slopkit::ui::components::MemoryView*>();
    REQUIRE(view != nullptr);
    viewer.set_modules({module_image("app", 0x100000, 0x1000)});
    viewer.set_address(0x100000);
    viewer.show();
    QCoreApplication::processEvents();

    // Answer the prompt with the chain text as soon as it opens.
    QTimer::singleShot(0,
                       []
                       {
                           for (QWidget* widget : QApplication::topLevelWidgets())
                           {
                               auto* box = qobject_cast<slopkit::ui::widgets::InputBox*>(widget);
                               if (box != nullptr && box->isVisible())
                               {
                                   box->line_edit()->setText(QStringLiteral("app+0+8"));
                                   box->accept();
                                   return;
                               }
                           }
                       });

    QMenu menu;
    view->populate_options_menu(menu);
    QAction* go_to = nullptr;
    for (QAction* action : menu.actions())
    {
        if (action->text() == QStringLiteral("Go To..."))
        {
            go_to = action;
            break;
        }
    }
    REQUIRE(go_to != nullptr);
    go_to->trigger();

    // The jump happens only once the worker resolves the chain.
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return view->first_byte() == 0x200008;
                    }));
    viewer.hide();
}

TEST_CASE("the memory viewer go-to reports an unreadable pointer level", "[ui]")
{
    application();

    slopkit::process::AttachedTarget target = fake_target();

    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};
    attach_app_session(worker);
    // The pointer's own address cannot be read, so the chain fails at level 1.
    access.unreadable->insert(0x100000);

    std::vector<slopkit::log::Record> records;
    SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                            {
                        records.push_back(record);
                                            }};

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target, shared_debug_controller()};
    auto*                                    view = viewer.findChild<slopkit::ui::components::MemoryView*>();
    REQUIRE(view != nullptr);
    viewer.set_modules({module_image("app", 0x100000, 0x1000)});
    viewer.set_address(0x100000);
    viewer.show();
    QCoreApplication::processEvents();

    QTimer::singleShot(0,
                       []
                       {
                           for (QWidget* widget : QApplication::topLevelWidgets())
                           {
                               auto* box = qobject_cast<slopkit::ui::widgets::InputBox*>(widget);
                               if (box != nullptr && box->isVisible())
                               {
                                   box->line_edit()->setText(QStringLiteral("app+0+8"));
                                   box->accept();
                                   return;
                               }
                           }
                       });

    QMenu menu;
    view->populate_options_menu(menu);
    QAction* go_to = nullptr;
    for (QAction* action : menu.actions())
    {
        if (action->text() == QStringLiteral("Go To..."))
        {
            go_to = action;
            break;
        }
    }
    REQUIRE(go_to != nullptr);
    go_to->trigger();

    // The failure is logged and the page does not move.
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        for (const slopkit::log::Record& record : records)
                        {
                            if (record.level == slopkit::log::Level::warning
                                && record.message.find("memory viewer go to failed") != std::string::npos)
                            {
                                return true;
                            }
                        }
                        return false;
                    }));
    CHECK(view->first_byte() == 0x100000);
    viewer.hide();
}

TEST_CASE("the memory viewer splits the window into the three panes", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    attach_app_session(worker);

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target, shared_debug_controller()};

    // A fresh viewer opens at 1280x960 but can still shrink to its 720x480 minimum.
    CHECK(viewer.size() == QSize(1280, 960));
    CHECK(viewer.minimumSize() == QSize(720, 480));

    viewer.show();
    for (std::size_t guard = 0; guard < 8; ++guard)
    {
        QCoreApplication::processEvents();
    }

    // The vertical split puts the upper zone (70 units) above the hex view (30).
    auto* split = viewer.findChild<QSplitter*>(QStringLiteral("viewer_split"));
    REQUIRE(split != nullptr);
    REQUIRE(split->count() == 2);
    const double upper_share =
        static_cast<double>(split->widget(0)->height()) / (split->widget(0)->height() + split->widget(1)->height());
    CHECK(upper_share > 0.63);
    CHECK(upper_share < 0.77);

    // The upper zone splits 70/30 between the listing and the debugger stats.
    auto* code_split = viewer.findChild<QSplitter*>(QStringLiteral("code_split"));
    REQUIRE(code_split != nullptr);
    REQUIRE(code_split->count() == 2);
    const double left_share = static_cast<double>(code_split->widget(0)->width())
                            / (code_split->widget(0)->width() + code_split->widget(1)->width());
    CHECK(left_share > 0.63);
    CHECK(left_share < 0.77);

    viewer.hide();
}

TEST_CASE("the memory viewer restores its size across a restart", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    slopkit::plugin::PluginHost      host;
    const QString                    settings_path = scratch_settings_file("viewer_geometry.ini");

    {
        slopkit::ui::SettingsController settings {settings_path};
        slopkit::ui::MainWindow         window {worker, target, host, settings, shared_debug_controller()};
        auto*                           viewer = window.memory_viewer();
        REQUIRE(viewer != nullptr);

        viewer->resize(740, 500);
        viewer->show();
        QCoreApplication::processEvents();
        REQUIRE(viewer->size() == QSize(740, 500));
        viewer->hide();
        QCoreApplication::processEvents();

        REQUIRE_FALSE(settings.values().memory_view_geometry.isEmpty());
    }

    {
        slopkit::ui::SettingsController settings {settings_path};
        REQUIRE_FALSE(settings.values().memory_view_geometry.isEmpty());
        slopkit::ui::MainWindow window {worker, target, host, settings, shared_debug_controller()};
        auto*                   viewer = window.memory_viewer();
        REQUIRE(viewer != nullptr);
        // The stored blob reopens the window at the size it was closed with.
        CHECK(viewer->size() == QSize(740, 500));
        CHECK(viewer->minimumSize() == QSize(720, 480));
    }
}

TEST_CASE("the memory viewer seeds both cursors and then moves them independently", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    attach_app_session(worker);

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target, shared_debug_controller()};
    auto*                                    view = viewer.findChild<slopkit::ui::components::MemoryView*>();
    REQUIRE(view != nullptr);
    auto* listing = viewer.findChild<slopkit::ui::components::DisassemblyView*>();
    REQUIRE(listing != nullptr);

    // Opening the viewer at an address seeds both panes.
    viewer.set_address(0x1234);
    CHECK(view->first_byte() == 0x1234);
    CHECK(listing->first_address() == 0x1234);

    // The byte view's Go To leaves the listing where it is.
    CHECK(viewer.go_to(QStringLiteral("0x2000")));
    CHECK(view->first_byte() == 0x2000);
    CHECK(listing->first_address() == 0x1234);

    // The listing's own Go To leaves the byte view where it is.
    CHECK(viewer.go_to_disassembly(QStringLiteral("0x3000")));
    CHECK(listing->first_address() == 0x3000);
    CHECK(view->first_byte() == 0x2000);
}

TEST_CASE("opening the memory viewer at an address clears both Back histories", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    attach_app_session(worker);

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target, shared_debug_controller()};
    auto*                                    view = viewer.findChild<slopkit::ui::components::MemoryView*>();
    REQUIRE(view != nullptr);
    auto* listing = viewer.findChild<slopkit::ui::components::DisassemblyView*>();
    REQUIRE(listing != nullptr);

    viewer.set_address(0x1000);
    view->navigate_to(0x2000);
    listing->navigate_to(0x3000);
    REQUIRE(view->can_go_back());
    REQUIRE(listing->can_go_back());

    // A fresh open-at address starts a clean navigation session in both panes.
    viewer.set_address(0x4000);
    CHECK_FALSE(view->can_go_back());
    CHECK_FALSE(listing->can_go_back());
    CHECK(view->first_byte() == 0x4000);
    CHECK(listing->first_address() == 0x4000);
}

TEST_CASE("the listing's Follow in Memory View moves only the byte view", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    attach_app_session(worker);

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target, shared_debug_controller()};
    auto*                                    view = viewer.findChild<slopkit::ui::components::MemoryView*>();
    REQUIRE(view != nullptr);
    auto* listing = viewer.findChild<slopkit::ui::components::DisassemblyView*>();
    REQUIRE(listing != nullptr);

    viewer.set_address(0x1000);
    view->set_first_byte(0x2000);
    REQUIRE(listing->first_address() == 0x1000);

    // "Follow in Memory View" sends the referenced address to the byte view and
    // leaves the listing where it is.
    emit listing->followInMemoryViewRequested(0x5000);
    CHECK(view->first_byte() == 0x5000);
    CHECK(listing->first_address() == 0x1000);
    REQUIRE(view->can_go_back());

    // The byte view's own Back returns it.
    QMenu    menu;
    QAction* back = nullptr;
    view->populate_options_menu(menu);
    for (QAction* candidate : menu.actions())
    {
        if (candidate->text() == QStringLiteral("Back"))
        {
            back = candidate;
        }
    }
    REQUIRE(back != nullptr);
    REQUIRE(back->isEnabled());
    back->trigger();
    CHECK(view->first_byte() == 0x2000);
    CHECK_FALSE(view->can_go_back());
}

TEST_CASE("the memory viewer routes its single Ctrl+G to the focused pane", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    attach_app_session(worker);

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target, shared_debug_controller()};
    auto*                                    view = viewer.findChild<slopkit::ui::components::MemoryView*>();
    REQUIRE(view != nullptr);
    auto* listing = viewer.findChild<slopkit::ui::components::DisassemblyView*>();
    REQUIRE(listing != nullptr);

    // The byte view owns the window's only Ctrl+G.
    REQUIRE(view->goto_action() != nullptr);
    CHECK(view->goto_action()->shortcut() == QKeySequence(QStringLiteral("Ctrl+G")));
    CHECK(listing->goto_action()->shortcut().isEmpty());

    viewer.set_address(0x1000);
    viewer.show();
    QCoreApplication::processEvents();

    const auto answer_prompt = [](const QString& text)
    {
        QTimer::singleShot(0,
                           [text]
                           {
                               for (QWidget* widget : QApplication::topLevelWidgets())
                               {
                                   auto* box = qobject_cast<slopkit::ui::widgets::InputBox*>(widget);
                                   if (box != nullptr && box->isVisible())
                                   {
                                       box->line_edit()->setText(text);
                                       box->accept();
                                       return;
                                   }
                               }
                           });
    };
    const auto trigger_ctrl_g = [&view]
    {
        QKeyEvent ctrl_g(QEvent::KeyPress, Qt::Key_G, Qt::ControlModifier);
        QApplication::sendEvent(view, &ctrl_g);
        QCoreApplication::processEvents();
    };

    // With the listing focused, the shared Ctrl+G moves the listing only.
    viewer.activateWindow();
    listing->setFocus();
    QCoreApplication::processEvents();
    REQUIRE(QApplication::focusWidget() == listing);
    answer_prompt(QStringLiteral("0x2000"));
    trigger_ctrl_g();
    CHECK(listing->first_address() == 0x2000);
    CHECK(view->first_byte() == 0x1000);

    // With the byte view focused, it moves the byte view instead.
    viewer.activateWindow(); // closing the prompt dropped the active window
    view->setFocus();
    QCoreApplication::processEvents();
    REQUIRE(QApplication::focusWidget() == view);
    answer_prompt(QStringLiteral("0x3000"));
    trigger_ctrl_g();
    CHECK(view->first_byte() == 0x3000);
    CHECK(listing->first_address() == 0x2000);

    viewer.hide();
}

TEST_CASE("the memory viewer shows a read-only register placeholder", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target, shared_debug_controller()};
    auto*                                    table = viewer.findChild<QTableView*>(QStringLiteral("register_table"));
    REQUIRE(table != nullptr);
    auto* model = qobject_cast<slopkit::ui::models::RegisterModel*>(table->model());
    REQUIRE(model != nullptr);

    using slopkit::ui::models::RegisterModel;

    const QString dash = QString(QChar(0x2014));
    CHECK(model->rowCount() == 18);
    CHECK(model->columnCount() == RegisterModel::column_count);
    CHECK(model->data(model->index(0, RegisterModel::name), Qt::DisplayRole).toString() == QStringLiteral("RAX"));
    CHECK(model->data(model->index(16, RegisterModel::name), Qt::DisplayRole).toString() == QStringLiteral("RIP"));
    CHECK(model->data(model->index(17, RegisterModel::name), Qt::DisplayRole).toString() == QStringLiteral("RFLAGS"));
    CHECK(model->data(model->index(0, RegisterModel::value), Qt::DisplayRole).toString() == dash);
    CHECK(model->data(model->index(17, RegisterModel::value), Qt::DisplayRole).toString() == dash);

    // Read-only: the debugger drives the values, not the user.
    CHECK_FALSE(model->flags(model->index(0, RegisterModel::value)) & Qt::ItemIsEditable);
    CHECK_FALSE(model->flags(model->index(0, RegisterModel::name)) & Qt::ItemIsEditable);

    // The pane is titled and carries the muted note.
    CHECK(panel_has_title(ancestor_panel(table), QStringLiteral("Debugger")));

    // The set_values seam the debugger will fill.
    const std::vector<slopkit::ui::models::RegisterValue> values {
        {.name = "RAX", .value = QStringLiteral("0x1"), .read = true}
    };
    model->set_values(values);
    CHECK(model->data(model->index(0, RegisterModel::value), Qt::DisplayRole).toString() == QStringLiteral("0x1"));
    CHECK(model->data(model->index(1, RegisterModel::value), Qt::DisplayRole).toString() == dash);
}

namespace
{
    using slopkit::tests::pump_until;

    // Puts `MOV EAX, [RBX+0x10]` at the front of the listing's code window and
    // applies one live pass, so row 0 is that instruction.
    std::uint64_t seed_listing_operand(FakeAccess& access, slopkit::ui::dialogs::MemoryViewerDialog& viewer)
    {
        const std::vector<slopkit::ui::LiveRequest> requests = viewer.next_live_request();
        std::vector<slopkit::ui::LiveReading>       readings;
        std::uint64_t                               code_base = 0;
        readings.reserve(requests.size());
        for (const slopkit::ui::LiveRequest& request : requests)
        {
            std::vector<std::byte> bytes(request.size, std::byte {0x90});
            if (request.id == slopkit::ui::components::DisassemblyDocument::kIdBase)
            {
                bytes[0]  = std::byte {0x8B};
                bytes[1]  = std::byte {0x43};
                bytes[2]  = std::byte {0x10};
                code_base = request.address;
            }
            (*access.memory)[request.address] = bytes;
            readings.push_back(slopkit::ui::LiveReading {request.id, true, std::move(bytes)});
        }
        viewer.apply_live_readings(readings);
        return code_base;
    }

    // Puts `PUSH RBP; MOV RBP, RSP; RET` at the front of the listing's code
    // window and applies one live pass, so rows 0..2 are those instructions. The
    // fake serves the bytes sparsely, because a window-only fake is read-only.
    std::uint64_t seed_listing_code(FakeAccess& access, slopkit::ui::dialogs::MemoryViewerDialog& viewer)
    {
        const std::vector<slopkit::ui::LiveRequest> requests = viewer.next_live_request();
        const std::vector<std::byte>                code {
            std::byte {0x55}, std::byte {0x48}, std::byte {0x89}, std::byte {0xE5}, std::byte {0xC3}};
        std::vector<slopkit::ui::LiveReading> readings;
        std::uint64_t                         code_base = 0;
        readings.reserve(requests.size());
        for (const slopkit::ui::LiveRequest& request : requests)
        {
            std::vector<std::byte> bytes(request.size, std::byte {0x90});
            if (request.id == slopkit::ui::components::DisassemblyDocument::kIdBase)
            {
                code_base = request.address;
                for (std::size_t index = 0; index < code.size(); ++index)
                {
                    bytes[index]                                  = code[index];
                    access.memory->bytes[request.address + index] = code[index];
                }
            }
            readings.push_back(slopkit::ui::LiveReading {request.id, true, std::move(bytes)});
        }
        viewer.apply_live_readings(readings);
        return code_base;
    }
} // namespace

TEST_CASE("the memory viewer resolves an instruction's memory operands", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    attach_app_session(worker);

    // A stopped session leaves the controller's register cache populated.
    slopkit::tests::FakeDebugBackend backend;
    slopkit::debug::Controller       controller {backend};
    controller.start(42, "fake");
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == slopkit::debug::Controller::State::stopped
                               && controller.registers().size() == 18;
                       }));
    REQUIRE(controller.registers().size() == 18);

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target, controller};
    viewer.set_address(0x1000);
    viewer.show();
    for (std::size_t guard = 0; guard < 8; ++guard)
    {
        QCoreApplication::processEvents();
    }
    const std::uint64_t code_base = seed_listing_operand(access, viewer);
    // Applying the readings only invalidates the document; the view decodes the
    // rows when it paints, so pump the event loop once more.
    for (std::size_t guard = 0; guard < 8; ++guard)
    {
        QCoreApplication::processEvents();
    }

    std::uint64_t                            instruction = 0;
    std::size_t                              length      = 0;
    std::vector<slopkit::ui::ResolvedAccess> resolved;
    QObject::connect(&viewer,
                     &slopkit::ui::dialogs::MemoryViewerDialog::instructionAccessesResolved,
                     &viewer,
                     [&](std::uint64_t address, std::size_t size, std::vector<slopkit::ui::ResolvedAccess> accesses)
                     {
                         instruction = address;
                         length      = size;
                         resolved    = std::move(accesses);
                     });

    viewer.instruction_accesses(0);
    // The listing decodes from its page-aligned window base, so row 0 is that
    // base; the operand resolves against the controller's cached registers.
    CHECK(instruction == code_base);
    CHECK(length == 3);
    REQUIRE(resolved.size() == 1);
    CHECK(resolved[0].operand == QStringLiteral("[RBX+10]"));
    CHECK(resolved[0].width == 4);
    CHECK_FALSE(resolved[0].writes);
    // The fake register file holds zeroes, so the operand resolves to the
    // instruction's own address plus the displacement.
    CHECK(resolved[0].resolved);
    CHECK(resolved[0].address == code_base + 0x10);

    // A running target whose register file cannot be read: the capture still
    // runs, the target is put back running, the rows arrive unresolved and a
    // warning is reported instead of a silent empty table.
    slopkit::tests::FakeDebugBackend plain_backend;
    plain_backend.register_file.clear();
    plain_backend.block_continue = true;
    slopkit::debug::Controller plain_controller {plain_backend};
    plain_controller.start(42, "fake");
    REQUIRE(pump_until(plain_controller,
                       [&]
                       {
                           return plain_controller.state() == slopkit::debug::Controller::State::running;
                       }));

    slopkit::ui::dialogs::MemoryViewerDialog plain {worker, target, plain_controller};
    plain.set_address(0x1000);
    plain.show();
    for (std::size_t guard = 0; guard < 8; ++guard)
    {
        QCoreApplication::processEvents();
    }
    seed_listing_operand(access, plain);

    int warnings = 0;
    resolved.clear();
    QObject::connect(&plain,
                     &slopkit::ui::dialogs::MemoryViewerDialog::instructionAccessesProgress,
                     &plain,
                     [&warnings](const QString&, bool error)
                     {
                         warnings += error ? 1 : 0;
                     });
    QObject::connect(&plain,
                     &slopkit::ui::dialogs::MemoryViewerDialog::instructionAccessesResolved,
                     &plain,
                     [&](std::uint64_t, std::size_t, std::vector<slopkit::ui::ResolvedAccess> accesses)
                     {
                         resolved = std::move(accesses);
                     });

    plain.instruction_accesses(0);
    REQUIRE(pump_until(plain_controller,
                       [&]
                       {
                           return !resolved.empty();
                       }));
    REQUIRE(resolved.size() == 1);
    CHECK_FALSE(resolved[0].resolved);
    CHECK(warnings == 1);
    // The invisible capture read the registers once and left the target running.
    CHECK(plain_backend.count("registers") == 1);
    CHECK(plain_controller.state() == slopkit::debug::Controller::State::running);

    // Every invocation captures again, so a repeated click reads the registers
    // once more and resolves against current values.
    resolved.clear();
    plain.instruction_accesses(0);
    REQUIRE(pump_until(plain_controller,
                       [&]
                       {
                           return !resolved.empty();
                       }));
    CHECK_FALSE(resolved[0].resolved);
    CHECK(plain_backend.count("registers") == 2);

    viewer.hide();
    plain.hide();
}

TEST_CASE("the memory viewer attaches the debugger on demand for the operands", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    attach_app_session(worker);

    slopkit::tests::FakeDebugBackend backend;
    backend.block_continue = true;
    slopkit::debug::Controller controller {backend};

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target, controller};
    viewer.set_address(0x1000);
    viewer.show();
    for (std::size_t guard = 0; guard < 8; ++guard)
    {
        QCoreApplication::processEvents();
    }
    const std::uint64_t code_base = seed_listing_operand(access, viewer);

    int prompts = 0;
    viewer.debug_gate().set_attach_prompt(
        [&prompts](const slopkit::process::AttachedTarget&, const QString& reason)
        {
            ++prompts;
            CHECK(reason == QStringLiteral("find out what this instruction accesses"));
            return true;
        });

    std::vector<slopkit::ui::ResolvedAccess> resolved;
    QObject::connect(&viewer,
                     &slopkit::ui::dialogs::MemoryViewerDialog::instructionAccessesResolved,
                     &viewer,
                     [&](std::uint64_t address, std::size_t, std::vector<slopkit::ui::ResolvedAccess> accesses)
                     {
                         CHECK(address == code_base);
                         resolved = std::move(accesses);
                     });

    CHECK(controller.state() == slopkit::debug::Controller::State::idle);
    viewer.instruction_accesses(0);
    CHECK(prompts == 1);
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           QCoreApplication::processEvents();
                           return !resolved.empty();
                       }));
    REQUIRE(resolved.size() == 1);
    // The fake register file holds zeroes, so the operand resolves, and the
    // session is left running.
    CHECK(resolved[0].resolved);
    CHECK(controller.state() == slopkit::debug::Controller::State::running);

    // A declined prompt never starts a session and yields no rows.
    slopkit::tests::FakeDebugBackend         denied_backend;
    slopkit::debug::Controller               denied_controller {denied_backend};
    slopkit::ui::dialogs::MemoryViewerDialog denied {worker, target, denied_controller};
    denied.set_address(0x1000);
    denied.show();
    for (std::size_t guard = 0; guard < 8; ++guard)
    {
        QCoreApplication::processEvents();
    }
    seed_listing_operand(access, denied);
    denied.debug_gate().set_attach_prompt(
        [](const slopkit::process::AttachedTarget&, const QString&)
        {
            return false;
        });
    bool denied_resolved = false;
    QObject::connect(&denied,
                     &slopkit::ui::dialogs::MemoryViewerDialog::instructionAccessesResolved,
                     &denied,
                     [&denied_resolved](std::uint64_t, std::size_t, std::vector<slopkit::ui::ResolvedAccess>)
                     {
                         denied_resolved = true;
                     });
    denied.instruction_accesses(0);
    CHECK_FALSE(denied_resolved);
    CHECK(denied_controller.state() == slopkit::debug::Controller::State::idle);
    CHECK(denied_backend.count("attach") == 0);

    viewer.hide();
    denied.hide();
}

TEST_CASE("the memory viewer puts the debug controls above the split", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    attach_app_session(worker);

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target, shared_debug_controller()};
    viewer.show();
    QCoreApplication::processEvents();

    auto* split = viewer.findChild<QSplitter*>(QStringLiteral("viewer_split"));
    REQUIRE(split != nullptr);
    REQUIRE(split->count() == 2);
    QWidget* upper = split->widget(0);

    auto* code_split = viewer.findChild<QSplitter*>(QStringLiteral("code_split"));
    REQUIRE(code_split != nullptr);
    REQUIRE(code_split->count() == 2);
    QWidget* stats_pane = code_split->widget(1);

    // The bar spans the whole width above the split, inside neither pane.
    auto* controls = viewer.findChild<slopkit::ui::panels::DebugControls*>();
    REQUIRE(controls != nullptr);
    CHECK(upper->isAncestorOf(controls));
    CHECK_FALSE(code_split->isAncestorOf(controls));
    CHECK_FALSE(stats_pane->isAncestorOf(controls));

    // It sits above the code split and reaches across the full pane width, so
    // all nine controls and the state line fit.
    CHECK(controls->parentWidget() == upper);
    CHECK(controls->y() < code_split->y());
    CHECK(controls->width() == code_split->width());

    // The bar carries all nine controls, with Step Out disabled.
    CHECK(controls->toggle_breakpoint_button() != nullptr);
    CHECK(controls->start_button() != nullptr);
    CHECK(controls->stop_button() != nullptr);
    CHECK(controls->resume_button() != nullptr);
    CHECK(controls->break_button() != nullptr);
    CHECK(controls->step_into_button() != nullptr);
    CHECK(controls->step_over_button() != nullptr);
    REQUIRE(controls->step_out_button() != nullptr);
    CHECK_FALSE(controls->step_out_button()->isEnabled());
    CHECK(controls->breakpoints_button() != nullptr);

    // The right pane holds the registers and the call stack and no run control.
    CHECK(stats_pane->findChildren<QPushButton*>().isEmpty());
    auto* register_table = stats_pane->findChild<QTableView*>(QStringLiteral("register_table"));
    REQUIRE(register_table != nullptr);
    auto* call_stack_table = stats_pane->findChild<QTableView*>(QStringLiteral("call_stack_table"));
    REQUIRE(call_stack_table != nullptr);

    viewer.hide();
}

TEST_CASE("the memory viewer listing NOPs and restores an instruction", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    attach_app_session(worker);

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target, shared_debug_controller()};
    auto*                                    listing = viewer.findChild<slopkit::ui::components::DisassemblyView*>();
    REQUIRE(listing != nullptr);

    viewer.set_address(0x2000);
    viewer.show();
    for (std::size_t guard = 0; guard < 8; ++guard)
    {
        QCoreApplication::processEvents();
    }
    const std::uint64_t code_base = seed_listing_code(access, viewer);
    for (std::size_t guard = 0; guard < 8; ++guard)
    {
        QCoreApplication::processEvents();
    }
    REQUIRE(listing->row_text(0) == QStringLiteral("PUSH RBP"));
    REQUIRE(listing->row_text(1) == QStringLiteral("MOV RBP, RSP"));

    const auto menu_action = [](QMenu& menu, const QString& prefix) -> QAction*
    {
        for (QAction* candidate : menu.actions())
        {
            if (candidate->text().startsWith(prefix))
            {
                return candidate;
            }
        }
        return nullptr;
    };

    // `NOP Instruction` on the MOV replaces its whole three-byte instruction.
    QMenu menu;
    listing->populate_menu(menu, 1);
    QAction* nop = menu_action(menu, QStringLiteral("NOP Instruction"));
    REQUIRE(nop != nullptr);
    nop->trigger();
    // Wait for the applied completion, not just the raw write: the listing only
    // repaints the new bytes once the document's callback has run.
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return listing->row_text(1) == QStringLiteral("NOP");
                    }));

    CHECK(access.memory->bytes.at(code_base) == std::byte {0x55});
    CHECK(access.memory->bytes.at(code_base + 2) == std::byte {0x90});
    CHECK(access.memory->bytes.at(code_base + 3) == std::byte {0x90});
    CHECK(access.memory->bytes.at(code_base + 4) == std::byte {0xC3});
    CHECK(listing->row_text(1) == QStringLiteral("NOP"));
    CHECK(listing->row_text(2) == QStringLiteral("NOP"));
    CHECK(listing->row_text(3) == QStringLiteral("NOP"));
    CHECK(listing->row_text(4) == QStringLiteral("RET"));

    // Right-clicking a later row of the run offers the restore, which writes the
    // original bytes back.
    QMenu patched;
    listing->populate_menu(patched, 2);
    QAction* restore = menu_action(patched, QStringLiteral("Restore Original Instruction at "));
    REQUIRE(restore != nullptr);
    restore->trigger();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return listing->row_text(1) == QStringLiteral("MOV RBP, RSP");
                    }));

    CHECK(access.memory->bytes.at(code_base + 2) == std::byte {0x89});
    CHECK(access.memory->bytes.at(code_base + 3) == std::byte {0xE5});
    CHECK(listing->row_text(1) == QStringLiteral("MOV RBP, RSP"));
    CHECK(listing->row_text(2) == QStringLiteral("RET"));

    viewer.hide();
}

TEST_CASE("the toggle breakpoint button sets and clears a breakpoint", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    attach_app_session(worker);

    slopkit::tests::FakeDebugBackend backend;
    slopkit::debug::Controller       controller {backend};

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target, controller};
    auto*                                    listing = viewer.findChild<slopkit::ui::components::DisassemblyView*>();
    REQUIRE(listing != nullptr);
    auto* controls = viewer.findChild<slopkit::ui::panels::DebugControls*>();
    REQUIRE(controls != nullptr);

    viewer.set_address(0x2000);
    viewer.show();
    for (std::size_t guard = 0; guard < 8; ++guard)
    {
        QCoreApplication::processEvents();
    }
    const std::uint64_t code_base = seed_listing_code(access, viewer);
    for (std::size_t guard = 0; guard < 8; ++guard)
    {
        QCoreApplication::processEvents();
    }
    REQUIRE(listing->row_text(0) == QStringLiteral("PUSH RBP"));

    // The toggle needs a live session: start one and wait for the stop.
    controls->start_button()->click();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == slopkit::debug::Controller::State::stopped;
                       }));

    // Select the first instruction: the bar's toggle comes live.
    listing->set_selected_address(code_base);
    REQUIRE(controls->toggle_breakpoint_button()->isEnabled());

    // The first click adds one software breakpoint at the selected address.
    controls->toggle_breakpoint_button()->click();
    REQUIRE(controller.table().software_at(code_base) != nullptr);
    CHECK(controller.breakpoints().size() == 1);

    // The second click removes it again.
    controls->toggle_breakpoint_button()->click();
    CHECK(controller.table().software_at(code_base) == nullptr);
    CHECK(controller.breakpoints().empty());

    viewer.hide();
}
