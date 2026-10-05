#include <catch2/catch.hpp>

#include <cmath>

#include <QWindow>

#include "support/ui_helpers.hpp"
#include "ui/components/disassembly_view.hpp"
#include "ui/models/register_model.hpp"

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
    slopkit::ui::MainWindow          window {worker, target, host, settings};

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

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target};
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

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target};
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

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target};
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

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target};
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

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target};
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

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target};
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

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target};

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
        slopkit::ui::MainWindow         window {worker, target, host, settings};
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
        slopkit::ui::MainWindow window {worker, target, host, settings};
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

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target};
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

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target};
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

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target};
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

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target};
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

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target};
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
