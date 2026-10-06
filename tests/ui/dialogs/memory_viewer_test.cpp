#include <catch2/catch.hpp>

#include <cmath>

#include <QAction>
#include <QMenu>
#include <QSplitter>
#include <QWindow>

#include <QPushButton>

#include "support/ui_helpers.hpp"
#include "ui/components/disassembly_view.hpp"
#include "ui/components/memory_view.hpp"
#include "ui/fonts.hpp"
#include "ui/models/register_model.hpp"
#include "ui/panels/debug_controls.hpp"
#include "ui/panels/viewer_menu.hpp"

namespace
{
    // Whether two live passes request exactly the same blocks.
    bool same_requests(const std::vector<slopkit::ui::LiveRequest>& left,
                       const std::vector<slopkit::ui::LiveRequest>& right)
    {
        if (left.size() != right.size())
        {
            return false;
        }
        for (std::size_t index = 0; index < left.size(); ++index)
        {
            if (left[index].id != right[index].id || left[index].address != right[index].address
                || left[index].size != right[index].size)
            {
                return false;
            }
        }
        return true;
    }

    // Waits until the dialog's live request set is identical on two consecutive
    // turns, i.e. until the show/layout pass has settled. The dialog may still
    // grow once after show(), and a slow layout that has not been delivered yet
    // must not be mistaken for a stable one.
    void settle_panes(slopkit::process::AccessWorker& worker, slopkit::ui::dialogs::MemoryViewerDialog& viewer)
    {
        std::vector<slopkit::ui::LiveRequest> previous = viewer.next_live_request();
        const bool                            settled  = pump_ui(worker,
                                                                 [&]
                                                                 {
                                         std::vector<slopkit::ui::LiveRequest> current = viewer.next_live_request();
                                         const bool                            same = same_requests(previous, current);
                                         previous                                   = std::move(current);
                                         return same;
                                                                 });
        REQUIRE(settled);
    }
} // namespace

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
    settle_panes(worker, viewer);
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
    settle_panes(worker, viewer);

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

    // The vertical split puts the upper zone (70 units) above the hex view (30).
    auto* split = viewer.findChild<QSplitter*>(QStringLiteral("viewer_split"));
    REQUIRE(split != nullptr);
    REQUIRE(split->count() == 2);
    // The panes get their final sizes in the show/layout pass; wait until the
    // geometry stops changing instead of pumping a fixed number of turns.
    int previous_height = -1;
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        const int  height  = split->widget(0)->height();
                        const bool settled = height > 0 && height == previous_height;
                        previous_height    = height;
                        return settled;
                    }));
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

    // The pane carries no `Debugger` title, while its two section headers still
    // name the tables inside it.
    CHECK_FALSE(panel_has_title(ancestor_panel(table), QStringLiteral("Debugger")));
    CHECK(panel_has_title(ancestor_panel(table), QStringLiteral("Registers")));
    CHECK(panel_has_title(ancestor_panel(table), QStringLiteral("Call stack")));

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

    // The dialog's live requests once its panes are up. A pane asks for its
    // window only while the dialog is shown, and the show/layout pass is not
    // synchronous with show(), so wait on the request itself: a fixed number of
    // processEvents() turns cannot guarantee it and the seeding pass below would
    // then be empty.
    std::vector<slopkit::ui::LiveRequest> wait_for_panes(slopkit::process::AccessWorker&           worker,
                                                         slopkit::ui::dialogs::MemoryViewerDialog& viewer)
    {
        std::vector<slopkit::ui::LiveRequest> requests;
        const auto                            shown = [&]
        {
            requests = viewer.next_live_request();
            for (const slopkit::ui::LiveRequest& request : requests)
            {
                if (request.id == slopkit::ui::components::DisassemblyDocument::kIdBase)
                {
                    return true;
                }
            }
            return false;
        };
        REQUIRE(pump_ui(worker, shown));
        return requests;
    }

    // Waits until the listing has decoded its first row, i.e. until the pass the
    // seeding helper applied has landed: `row_text(0)` is empty while it has none.
    void wait_for_listing_row(slopkit::process::AccessWorker& worker, slopkit::ui::dialogs::MemoryViewerDialog& viewer)
    {
        auto* listing = viewer.findChild<slopkit::ui::components::DisassemblyView*>();
        REQUIRE(listing != nullptr);
        REQUIRE(pump_ui(worker,
                        [&]
                        {
                            return !listing->row_text(0).isEmpty();
                        }));
    }

    // Puts `MOV EAX, [RBX+0x10]` at the front of the listing's code window and
    // applies one live pass, so row 0 is that instruction.
    std::uint64_t seed_listing_operand(FakeAccess&                               access,
                                       slopkit::process::AccessWorker&           worker,
                                       slopkit::ui::dialogs::MemoryViewerDialog& viewer)
    {
        const std::vector<slopkit::ui::LiveRequest> requests = wait_for_panes(worker, viewer);
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
        wait_for_listing_row(worker, viewer);
        return code_base;
    }

    // Puts `PUSH RBP; MOV RBP, RSP; RET` at the front of the listing's code
    // window and applies one live pass, so rows 0..2 are those instructions. The
    // fake serves the bytes sparsely, because a window-only fake is read-only.
    std::uint64_t seed_listing_code(FakeAccess&                               access,
                                    slopkit::process::AccessWorker&           worker,
                                    slopkit::ui::dialogs::MemoryViewerDialog& viewer)
    {
        const std::vector<slopkit::ui::LiveRequest> requests = wait_for_panes(worker, viewer);
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
        wait_for_listing_row(worker, viewer);
        return code_base;
    }

    // Puts `JMP <+7>` at the front of the listing's code window and applies one
    // live pass, so row 0 is a branch that references an address.
    std::uint64_t seed_listing_jump(FakeAccess&                               access,
                                    slopkit::process::AccessWorker&           worker,
                                    slopkit::ui::dialogs::MemoryViewerDialog& viewer)
    {
        const std::vector<slopkit::ui::LiveRequest> requests = wait_for_panes(worker, viewer);
        const std::vector<std::byte>                code {std::byte {0xEB},
                                                          std::byte {0x05},
                                                          std::byte {0x90},
                                                          std::byte {0x90},
                                                          std::byte {0x90},
                                                          std::byte {0x90},
                                                          std::byte {0x90}};
        std::vector<slopkit::ui::LiveReading>       readings;
        std::uint64_t                               code_base = 0;
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
        wait_for_listing_row(worker, viewer);
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
    const std::uint64_t code_base = seed_listing_operand(access, worker, viewer);

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
    plain_backend.set_register_file({});
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
    seed_listing_operand(access, worker, plain);

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
    const std::uint64_t code_base = seed_listing_operand(access, worker, viewer);

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
    seed_listing_operand(access, worker, denied);
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

TEST_CASE("the memory viewer puts the debug controls in the listing column", "[ui]")
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

    auto* code_split = viewer.findChild<QSplitter*>(QStringLiteral("code_split"));
    REQUIRE(code_split != nullptr);
    REQUIRE(code_split->count() == 2);
    REQUIRE(split->widget(0) == code_split);
    QWidget* code_pane  = code_split->widget(0);
    QWidget* stats_pane = code_split->widget(1);

    // The bar belongs to the listing column: it is a child of that column and
    // shares its width, and it never reaches over the registers pane.
    auto* controls = viewer.findChild<slopkit::ui::panels::DebugControls*>();
    REQUIRE(controls != nullptr);
    CHECK(code_pane->isAncestorOf(controls));
    CHECK(controls->parentWidget() == code_pane);
    CHECK_FALSE(stats_pane->isAncestorOf(controls));
    CHECK(controls->width() == code_pane->width());

    // Its right edge stays left of the stats pane's left edge.
    const int controls_right = controls->mapTo(&viewer, QPoint(controls->width(), 0)).x();
    const int stats_left     = stats_pane->mapTo(&viewer, QPoint(0, 0)).x();
    CHECK(controls_right <= stats_left);

    // It sits above the Disassembly panel inside that column.
    auto* listing_panel = code_pane->findChild<slopkit::ui::widgets::Panel*>();
    REQUIRE(listing_panel != nullptr);
    CHECK(listing_panel->isAncestorOf(code_pane->findChild<slopkit::ui::components::DisassemblyView*>()));
    CHECK(controls->y() < listing_panel->y());

    // The bar carries all nine controls, with Step Out disabled.
    CHECK(controls->toggle_breakpoint_button() != nullptr);
    CHECK(controls->start_stop_button() != nullptr);
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
    const std::uint64_t code_base = seed_listing_code(access, worker, viewer);
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
    const std::uint64_t code_base = seed_listing_code(access, worker, viewer);
    REQUIRE(listing->row_text(0) == QStringLiteral("PUSH RBP"));

    // The toggle needs a live session: start one and wait for the stop.
    controls->start_stop_button()->click();
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

TEST_CASE("the memory viewer shows the debugger read-out on its bottom status line", "[ui]")
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
    viewer.show();
    QCoreApplication::processEvents();

    auto* status = viewer.findChild<slopkit::ui::widgets::StatusLabel*>(QStringLiteral("viewer_status"));
    REQUIRE(status != nullptr);

    // The line is the mono read-out and starts at the controller's idle text.
    CHECK(status->font().family() == slopkit::ui::mono_font().family());
    CHECK(viewer.status_text() == QStringLiteral("Not debugging"));

    // Starting the session updates the line through the controller's signal.
    controller.start(target.pid, target.plugin_id);
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == slopkit::debug::Controller::State::running;
                       }));
    CHECK(viewer.status_text() == QStringLiteral("Running..."));

    // A deliberate stop shows where the target stopped.
    controller.interrupt();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == slopkit::debug::Controller::State::stopped;
                       }));
    CHECK(viewer.status_text().startsWith(QStringLiteral("Stopped at")));

    // A controller message replaces it and is coloured by its kind.
    controller.start(target.pid, target.plugin_id);
    CHECK(viewer.status_text() == QStringLiteral("A debug session is already open."));
    CHECK(status->palette().color(QPalette::WindowText)
          == slopkit::ui::widgets::status_color(slopkit::ui::widgets::StatusKind::warning));

    viewer.hide();
}

TEST_CASE("the memory viewer shows its own four-menu bar above the panes", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    attach_app_session(worker);

    slopkit::tests::FakeDebugBackend backend;
    slopkit::debug::Controller       controller {backend};

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target, controller};
    auto*                                    menu = viewer.viewer_menu();
    auto*                                    view = viewer.findChild<slopkit::ui::components::MemoryView*>();
    REQUIRE(menu != nullptr);
    REQUIRE(view != nullptr);
    CHECK(viewer.findChild<slopkit::ui::panels::ViewerMenu*>() == menu);

    // Exactly the four menus, in the required order.
    REQUIRE(menu->actions().size() == 4);
    CHECK(action_texts(menu->actions())
          == QStringList {
              QStringLiteral("File"), QStringLiteral("View"), QStringLiteral("Tools"), QStringLiteral("Debug")});

    // The Debug menu carries the bar's entries plus Remove All Breakpoints.
    CHECK(action_texts(menu->debug_menu()->actions())
          == QStringList {QStringLiteral("Start Debugging"),
                          QStringLiteral("Toggle Breakpoint"),
                          QStringLiteral("Resume"),
                          QStringLiteral("Break"),
                          QStringLiteral("Step Into"),
                          QStringLiteral("Step Over"),
                          QStringLiteral("Step Out"),
                          QStringLiteral("Breakpoints..."),
                          QStringLiteral("Remove All Breakpoints")});

    // The byte view's own Go To... action leads the View menu, so its Ctrl+G is
    // never duplicated.
    REQUIRE_FALSE(menu->view_menu()->actions().isEmpty());
    CHECK(menu->view_menu()->actions().first() == view->goto_action());

    // The bar sits above the panes.
    viewer.resize(900, 600);
    viewer.show();
    QCoreApplication::processEvents();
    auto* split = viewer.findChild<QSplitter*>();
    REQUIRE(split != nullptr);
    CHECK(menu->y() < split->y());
    viewer.hide();
}

TEST_CASE("the debug menu mirrors the control bar across the session states", "[ui]")
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
    auto*                                    listing  = viewer.findChild<slopkit::ui::components::DisassemblyView*>();
    auto*                                    controls = viewer.debug_controls();
    auto*                                    menu     = viewer.viewer_menu();
    REQUIRE(listing != nullptr);
    REQUIRE(controls != nullptr);
    REQUIRE(menu != nullptr);

    // Idle with a valid target: Start is live, the run entries are not.
    CHECK(menu->start_stop_action()->text() == QStringLiteral("Start Debugging"));
    CHECK(menu->start_stop_action()->isEnabled());
    CHECK_FALSE(menu->resume_action()->isEnabled());
    CHECK_FALSE(menu->break_action()->isEnabled());
    CHECK_FALSE(menu->step_into_action()->isEnabled());
    CHECK_FALSE(menu->step_over_action()->isEnabled());
    CHECK_FALSE(menu->step_out_action()->isEnabled());
    CHECK(menu->step_out_action()->toolTip() == QStringLiteral("Stepping out is not supported yet."));

    viewer.set_address(0x2000);
    viewer.show();
    const std::uint64_t code_base = seed_listing_code(access, worker, viewer);
    REQUIRE(listing->row_text(0) == QStringLiteral("PUSH RBP"));
    const QString selected_text = QStringLiteral("%1").arg(code_base, 16, 16, QLatin1Char('0')).toUpper();

    // Every debug entry agrees with its bar button in every state.
    const auto parity = [&]
    {
        CHECK(menu->resume_action()->isEnabled() == controls->resume_button()->isEnabled());
        CHECK(menu->break_action()->isEnabled() == controls->break_button()->isEnabled());
        CHECK(menu->step_into_action()->isEnabled() == controls->step_into_button()->isEnabled());
        CHECK(menu->step_over_action()->isEnabled() == controls->step_over_button()->isEnabled());
        CHECK(menu->toggle_breakpoint_action()->isEnabled() == controls->toggle_breakpoint_button()->isEnabled());
    };

    // No session: the toggle explains it cannot set breakpoints yet.
    CHECK_FALSE(menu->toggle_breakpoint_action()->isEnabled());
    CHECK(menu->toggle_breakpoint_action()->toolTip() == QStringLiteral("Start the debugger to set breakpoints."));
    parity();

    // Starting through the menu entry flips it to Stop and lights Break.
    menu->start_stop_action()->trigger();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == slopkit::debug::Controller::State::running;
                       }));
    CHECK(menu->start_stop_action()->text() == QStringLiteral("Stop Debugging"));
    CHECK(menu->start_stop_action()->isEnabled());
    CHECK(menu->break_action()->isEnabled());
    CHECK_FALSE(menu->resume_action()->isEnabled());
    CHECK(menu->toggle_breakpoint_action()->toolTip() == QStringLiteral("Select an instruction in the listing first."));
    parity();

    // Break through the menu stops the target and lights the step entries.
    menu->break_action()->trigger();
    REQUIRE(pump_until(controller,
                       [&]
                       {
                           return controller.state() == slopkit::debug::Controller::State::stopped;
                       }));
    CHECK(menu->resume_action()->isEnabled());
    CHECK(menu->step_into_action()->isEnabled());
    CHECK(menu->step_over_action()->isEnabled());
    CHECK_FALSE(menu->break_action()->isEnabled());
    parity();

    // Selecting an instruction lights the toggle on both surfaces.
    listing->set_selected_address(code_base);
    CHECK(menu->toggle_breakpoint_action()->isEnabled());
    CHECK(menu->toggle_breakpoint_action()->toolTip() == QStringLiteral("Set a breakpoint at %1.").arg(selected_text));
    parity();

    // Toggling through the menu arms the same address the bar would.
    menu->toggle_breakpoint_action()->trigger();
    REQUIRE(controller.table().software_at(code_base) != nullptr);
    CHECK(menu->toggle_breakpoint_action()->toolTip()
          == QStringLiteral("Remove the breakpoint at %1.").arg(selected_text));

    // A second breakpoint on the next row: Remove All clears both at once.
    listing->set_selected_address(code_base + 1);
    menu->toggle_breakpoint_action()->trigger();
    REQUIRE(controller.breakpoints().size() == 2);
    CHECK(menu->remove_all_breakpoints_action()->isEnabled());
    menu->remove_all_breakpoints_action()->trigger();
    CHECK(controller.table().software_at(code_base) == nullptr);
    CHECK(controller.table().software_at(code_base + 1) == nullptr);
    CHECK(controller.breakpoints().empty());
    CHECK_FALSE(menu->remove_all_breakpoints_action()->isEnabled());
    CHECK(menu->remove_all_breakpoints_action()->toolTip() == QStringLiteral("No breakpoints to remove."));

    // Breakpoints... reaches the same request the bar's button emits.
    int requests = 0;
    QObject::connect(controls,
                     &slopkit::ui::panels::DebugControls::breakpointsRequested,
                     controls,
                     [&requests]
                     {
                         ++requests;
                     });
    menu->breakpoints_action()->trigger();
    CHECK(requests == 1);

    viewer.hide();
}

TEST_CASE("the menu bar's Tools entries track the selected instruction", "[ui]")
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
    auto*                                    menu    = viewer.viewer_menu();
    REQUIRE(listing != nullptr);
    REQUIRE(menu != nullptr);

    viewer.set_address(0x2000);
    viewer.show();
    const std::uint64_t code_base = seed_listing_operand(access, worker, viewer);
    REQUIRE_FALSE(listing->row_text(0).isEmpty());
    CHECK(listing->row_text(0).startsWith(QStringLiteral("MOV EAX")));

    // Nothing selected: every Tools entry is off and explains itself.
    CHECK_FALSE(menu->nop_action()->isEnabled());
    CHECK_FALSE(menu->restore_action()->isEnabled());
    CHECK_FALSE(menu->edit_action()->isEnabled());
    CHECK_FALSE(menu->instruction_accesses_action()->isEnabled());
    CHECK_FALSE(menu->nop_action()->toolTip().isEmpty());
    CHECK_FALSE(menu->edit_action()->toolTip().isEmpty());
    CHECK_FALSE(menu->instruction_accesses_action()->toolTip().isEmpty());

    // A decoded instruction with a memory operand can be NOPed, edited and its
    // operands resolved.
    listing->set_selected_address(code_base);
    CHECK(menu->nop_action()->isEnabled());
    CHECK(menu->edit_action()->isEnabled());
    CHECK_FALSE(menu->restore_action()->isEnabled());
    CHECK(menu->instruction_accesses_action()->isEnabled());

    // NOPing it makes the row restorable and no longer editable. The selection
    // is dropped and re-made because reselecting the same row emits nothing.
    menu->nop_action()->trigger();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return listing->row_text(0) == QStringLiteral("NOP");
                    }));
    listing->set_selected_address(std::nullopt);
    listing->set_selected_address(code_base);
    CHECK_FALSE(menu->nop_action()->isEnabled());
    CHECK_FALSE(menu->edit_action()->isEnabled());
    CHECK(menu->restore_action()->isEnabled());
    CHECK(menu->restore_action()->toolTip().contains(QStringLiteral("MOV EAX")));

    viewer.hide();
}

TEST_CASE("the menu bar's viewer commands act on the panes", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    attach_app_session(worker);

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target, shared_debug_controller()};
    auto*                                    view    = viewer.findChild<slopkit::ui::components::MemoryView*>();
    auto*                                    listing = viewer.findChild<slopkit::ui::components::DisassemblyView*>();
    auto*                                    menu    = viewer.viewer_menu();
    REQUIRE(view != nullptr);
    REQUIRE(listing != nullptr);
    REQUIRE(menu != nullptr);

    viewer.set_address(0x1000);
    viewer.show();
    QCoreApplication::processEvents();

    // Neither pane has anywhere to go back to yet.
    CHECK_FALSE(menu->back_action()->isEnabled());
    CHECK_FALSE(menu->back_action()->toolTip().isEmpty());

    // With nothing selected the Follow entries are off and explain why.
    CHECK_FALSE(menu->follow_action()->isEnabled());
    CHECK_FALSE(menu->follow_in_memory_view_action()->isEnabled());
    CHECK(menu->follow_action()->toolTip() == QStringLiteral("Select an instruction that references an address."));

    // The byte view's own Back moves only the focused byte view.
    view->navigate_to(0x2000);
    REQUIRE(view->can_go_back());
    menu->view_menu()->aboutToShow();
    REQUIRE(menu->back_action()->isEnabled());
    menu->back_action()->trigger();
    CHECK(view->first_byte() == 0x1000);
    CHECK(listing->first_address() == 0x1000);

    // A selected row that references an address enables Follow: it moves the
    // byte view and, separately, the listing.
    const std::uint64_t jump_base = seed_listing_jump(access, worker, viewer);
    listing->set_selected_address(jump_base);
    menu->view_menu()->aboutToShow();
    REQUIRE(menu->follow_in_memory_view_action()->isEnabled());
    menu->follow_in_memory_view_action()->trigger();
    CHECK(view->first_byte() == jump_base + 7);

    listing->set_selected_address(jump_base);
    menu->view_menu()->aboutToShow();
    REQUIRE(menu->follow_action()->isEnabled());
    menu->follow_action()->trigger();
    CHECK(listing->can_go_back());

    // File > Close hides the viewer.
    CHECK(viewer.isVisible());
    menu->close_action()->trigger();
    CHECK_FALSE(viewer.isVisible());
}

TEST_CASE("the viewer re-gates its Start/Stop surfaces when the attached target changes", "[ui]")
{
    application();

    FakeAccess                     access;
    slopkit::process::AccessWorker worker {access};

    // Built before any attach: both Start/Stop surfaces read Start and are off.
    slopkit::process::AttachedTarget         target;
    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target, shared_debug_controller()};
    auto*                                    controls = viewer.debug_controls();
    auto*                                    menu     = viewer.viewer_menu();
    REQUIRE(controls != nullptr);
    REQUIRE(menu != nullptr);
    CHECK(controls->start_stop_button()->text() == QStringLiteral("Start Debugging"));
    CHECK_FALSE(controls->start_stop_button()->isEnabled());
    CHECK_FALSE(menu->start_stop_action()->isEnabled());

    // A fresh attach makes the target valid; no controller signal announces it,
    // so the explicit refresh is what lights both surfaces up.
    target.pid          = 4242;
    target.plugin_id    = "linux-proc";
    target.session_live = true;
    viewer.refresh_target_state();
    CHECK(controls->start_stop_button()->isEnabled());
    CHECK(menu->start_stop_action()->isEnabled());

    // Detaching again turns them off, matching the window's own label.
    target.clear();
    viewer.refresh_target_state();
    CHECK_FALSE(controls->start_stop_button()->isEnabled());
    CHECK_FALSE(menu->start_stop_action()->isEnabled());
}
