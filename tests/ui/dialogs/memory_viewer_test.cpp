#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

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

    auto* viewer = window.findChild<slopkit::ui::dialogs::MemoryViewerDialog*>();
    REQUIRE(viewer != nullptr);
    auto* view = viewer->findChild<slopkit::ui::components::MemoryView*>();
    REQUIRE(view != nullptr);
    auto* document = viewer->findChild<slopkit::ui::components::MemoryViewDocument*>();
    REQUIRE(document != nullptr);

    memory_view->click();
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

    // The byte view is the whole dialog: no loading or status row remains.
    CHECK(viewer.findChild<QLabel*>(QStringLiteral("loading_label")) == nullptr);
    CHECK(viewer.findChildren<slopkit::ui::widgets::StatusLabel*>().isEmpty());

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
    REQUIRE(requests.size() == 3);
    const std::uint64_t extent = requests[1].size;

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
