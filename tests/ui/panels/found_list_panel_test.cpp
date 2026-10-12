#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

TEST_CASE("the found list keeps one result line", "[ui]")
{
    application();

    slopkit::scan::ScanEngine           idle_engine;
    slopkit::table::AddressTable        idle_table;
    slopkit::ui::panels::FoundListPanel idle_panel {idle_engine, idle_table};
    auto*                               idle_header = idle_panel.findChild<QLabel*>();
    REQUIRE(idle_header != nullptr);
    CHECK(idle_header->text() == QStringLiteral("Showing 0 of 0 results"));
    CHECK(idle_panel.findChildren<QLabel*>().size() == 1);

    // A finished scan keeps the same line, whatever the result count, instead of
    // adding a second status row.
    std::vector<std::byte> bytes(12, std::byte {0});
    for (std::size_t i = 0; i < 3; ++i)
    {
        const std::uint32_t value = 10;
        std::memcpy(bytes.data() + i * 4, &value, sizeof(value));
    }

    slopkit::scan::ScanEngine engine;
    slopkit::scan::ScanConfig config;
    config.value = std::int64_t {10};
    engine.first_scan(config, slopkit::scan::make_buffer_source(bytes, 0x1000));
    for (int i = 0; i < 5000 && engine.is_running(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(engine.has_results());
    const auto snapshot = engine.snapshot();
    REQUIRE(snapshot.hits.size() == 3);
    REQUIRE(snapshot.hit_count == 3);

    slopkit::table::AddressTable        table;
    slopkit::ui::panels::FoundListPanel panel {engine, table};
    panel.refresh();

    auto* header = panel.findChild<QLabel*>();
    REQUIRE(header != nullptr);
    CHECK(header->text() == QStringLiteral("Showing 3 of 3 results"));
    CHECK(panel.findChildren<QLabel*>().size() == 1);
}

TEST_CASE("the found list stays empty until the scan finishes", "[ui]")
{
    application();

    auto bytes = std::make_shared<std::vector<std::byte>>(300 * 4, std::byte {0});
    for (std::size_t i = 0; i < 300; ++i)
    {
        const std::uint32_t value = 10;
        std::memcpy(bytes->data() + i * 4, &value, sizeof(value));
    }

    auto                        release = std::make_shared<std::atomic<bool>>(false);
    slopkit::scan::MemorySource source;
    source.read = [bytes,
                   release](std::uint64_t address,
                            std::size_t   size) -> std::expected<std::vector<std::byte>, slopkit::process::AccessError>
    {
        while (!release->load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (address < 0x1000 || address - 0x1000 + size > bytes->size())
        {
            return std::unexpected(slopkit::process::AccessError::not_found);
        }
        const auto offset = static_cast<std::size_t>(address - 0x1000);
        return std::vector<std::byte>(bytes->begin() + static_cast<std::ptrdiff_t>(offset),
                                      bytes->begin() + static_cast<std::ptrdiff_t>(offset + size));
    };
    source.regions = [bytes]()
    {
        slopkit::process::RegionInfo region;
        region.start    = 0x1000;
        region.end      = 0x1000 + bytes->size();
        region.readable = true;
        region.writable = true;
        return std::vector<slopkit::process::RegionInfo> {region};
    };

    slopkit::scan::ScanEngine engine;
    slopkit::scan::ScanConfig config;
    config.value = std::int64_t {10};
    engine.first_scan(config, source);
    REQUIRE(engine.is_running());

    slopkit::table::AddressTable        table;
    slopkit::ui::panels::FoundListPanel panel {engine, table};
    auto*                               view = panel.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    auto* model = dynamic_cast<slopkit::ui::models::FoundResultsModel*>(view->model());
    REQUIRE(model != nullptr);
    auto* header = panel.findChild<QLabel*>();
    REQUIRE(header != nullptr);

    panel.refresh();
    CHECK(model->rowCount() == 0);
    CHECK(header->text().startsWith(QStringLiteral("Scanning... ")));
    CHECK(view->selectionModel()->selectedRows().isEmpty());

    release->store(true);
    for (int i = 0; i < 5000 && engine.is_running(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE_FALSE(engine.is_running());
    REQUIRE(engine.has_results());
    panel.refresh();
    CHECK(model->rowCount() == slopkit::scan::kDisplayPage);
    CHECK(header->text() == QStringLiteral("Showing 256 of 300 results"));
}

TEST_CASE("the found list keeps the rows when a refinement is cancelled", "[ui]")
{
    application();

    auto bytes = std::make_shared<std::vector<std::byte>>(300 * 4, std::byte {0});
    for (std::size_t i = 0; i < 300; ++i)
    {
        const std::uint32_t value = 10;
        std::memcpy(bytes->data() + i * 4, &value, sizeof(value));
    }

    auto                        block   = std::make_shared<std::atomic<bool>>(false);
    auto                        release = std::make_shared<std::atomic<bool>>(false);
    slopkit::scan::MemorySource source;
    source.read = [bytes, block, release](
                      std::uint64_t address,
                      std::size_t   size) -> std::expected<std::vector<std::byte>, slopkit::process::AccessError>
    {
        if (block->load())
        {
            while (!release->load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (address < 0x1000 || address - 0x1000 + size > bytes->size())
        {
            return std::unexpected(slopkit::process::AccessError::not_found);
        }
        const auto offset = static_cast<std::size_t>(address - 0x1000);
        return std::vector<std::byte>(bytes->begin() + static_cast<std::ptrdiff_t>(offset),
                                      bytes->begin() + static_cast<std::ptrdiff_t>(offset + size));
    };
    source.regions = [bytes]()
    {
        slopkit::process::RegionInfo region;
        region.start    = 0x1000;
        region.end      = 0x1000 + bytes->size();
        region.readable = true;
        region.writable = true;
        return std::vector<slopkit::process::RegionInfo> {region};
    };

    slopkit::scan::ScanEngine engine;
    slopkit::scan::ScanConfig config;
    config.value = std::int64_t {10};
    engine.first_scan(config, source);
    for (int i = 0; i < 5000 && engine.is_running(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(engine.has_results());

    slopkit::table::AddressTable        table;
    slopkit::ui::panels::FoundListPanel panel {engine, table};
    auto*                               view = panel.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    auto* model = dynamic_cast<slopkit::ui::models::FoundResultsModel*>(view->model());
    REQUIRE(model != nullptr);
    auto* header = panel.findChild<QLabel*>();
    REQUIRE(header != nullptr);

    panel.refresh();
    REQUIRE(model->rowCount() == slopkit::scan::kDisplayPage);
    REQUIRE(header->text() == QStringLiteral("Showing 256 of 300 results"));

    block->store(true);
    slopkit::scan::ScanConfig refinement;
    refinement.type       = slopkit::scan::ScanType::unchanged;
    refinement.value_type = slopkit::scan::ValueType::int32;
    engine.next_scan(refinement);
    REQUIRE(engine.is_running());

    panel.refresh();
    CHECK(model->rowCount() == 0);
    CHECK(header->text().startsWith(QStringLiteral("Scanning... ")));

    engine.cancel();
    release->store(true);
    for (int i = 0; i < 5000 && engine.is_running(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE_FALSE(engine.is_running());

    panel.refresh();
    CHECK(model->rowCount() == slopkit::scan::kDisplayPage);
    CHECK(header->text() == QStringLiteral("Showing 256 of 300 results"));
}

TEST_CASE("the found list row menu carries the copy submenu", "[ui]")
{
    application();

    slopkit::scan::ScanEngine           engine;
    slopkit::table::AddressTable        table;
    slopkit::ui::panels::FoundListPanel panel {engine, table};

    QMenu menu;
    panel.populate_row_menu(menu, 0);
    CHECK(menu.toolTipsVisible());

    CHECK(action_texts(menu.actions())
          == QList<QString> {QStringLiteral("Add to address table"),
                             QStringLiteral("Change value…"),
                             QStringLiteral("Copy"),
                             QStringLiteral("Browse this memory region"),
                             QStringLiteral("Watch accesses")});

    QMenu* copy = submenu_of(menu, QStringLiteral("Copy"));
    REQUIRE(copy != nullptr);
    CHECK(copy->toolTipsVisible());
    CHECK(action_texts(copy->actions())
          == QList<QString> {QStringLiteral("Address (module + RVA)"),
                             QStringLiteral("Address (absolute)"),
                             QStringLiteral("Address + value")});

    QMenu* watch = submenu_of(menu, QStringLiteral("Watch accesses"));
    REQUIRE(watch != nullptr);
    CHECK(watch->toolTipsVisible());

    QAction* watch_writes   = nullptr;
    QAction* watch_accesses = nullptr;
    for (QAction* action : watch->actions())
    {
        if (action->text() == QStringLiteral("Find out what writes this address"))
        {
            watch_writes = action;
        }
        if (action->text() == QStringLiteral("Find out what accesses this address"))
        {
            watch_accesses = action;
        }
    }
    REQUIRE(watch_writes != nullptr);
    REQUIRE(watch_accesses != nullptr);
    CHECK_FALSE(watch_writes->isEnabled());
    CHECK_FALSE(watch_accesses->isEnabled());
    CHECK(watch_writes->toolTip() == QStringLiteral("Attach to a target first."));

    QAction* change = nullptr;
    QAction* browse = nullptr;
    for (QAction* action : menu.actions())
    {
        if (action->text() == QStringLiteral("Change value…"))
        {
            change = action;
        }
        if (action->text() == QStringLiteral("Browse this memory region"))
        {
            browse = action;
        }
    }
    REQUIRE(change != nullptr);
    REQUIRE(browse != nullptr);
    CHECK_FALSE(change->isEnabled());
    CHECK(change->toolTip() == QStringLiteral("Attach to a target first."));
    CHECK(browse->isEnabled());

    CHECK(change->shortcut() == QKeySequence(QStringLiteral("Ctrl+E")));
    CHECK(change->shortcutContext() == Qt::WidgetShortcut);
    CHECK(browse->shortcut() == QKeySequence(QStringLiteral("Ctrl+D")));
    CHECK(browse->shortcutContext() == Qt::WidgetShortcut);

    auto* view = panel.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    QAction* shortcut_edit   = nullptr;
    QAction* shortcut_browse = nullptr;
    for (QAction* action : view->actions())
    {
        if (action->shortcut() == QKeySequence(QStringLiteral("Ctrl+E")))
        {
            shortcut_edit = action;
        }
        if (action->shortcut() == QKeySequence(QStringLiteral("Ctrl+D")))
        {
            shortcut_browse = action;
        }
    }
    REQUIRE(shortcut_edit != nullptr);
    REQUIRE(shortcut_browse != nullptr);
    CHECK(shortcut_edit->text() == QStringLiteral("Change value…"));
    CHECK(shortcut_browse->text() == QStringLiteral("Browse this memory region"));
    CHECK(shortcut_edit->shortcutContext() == Qt::WidgetWithChildrenShortcut);
    CHECK(shortcut_browse->shortcutContext() == Qt::WidgetWithChildrenShortcut);

    menu.actions().front()->trigger();
    copy->actions().front()->trigger();
    browse->trigger();
}

TEST_CASE("the found list changes the value of the whole selection", "[ui]")
{
    application();

    std::vector<std::byte> bytes(12, std::byte {0});
    for (std::size_t i = 0; i < 3; ++i)
    {
        const std::uint32_t value = 10;
        std::memcpy(bytes.data() + static_cast<std::ptrdiff_t>(i * 4), &value, sizeof(value));
    }

    slopkit::scan::ScanEngine engine;
    slopkit::scan::ScanConfig config;
    config.value = std::int64_t {10};
    engine.first_scan(config, slopkit::scan::make_buffer_source(bytes, 0x1000));
    for (int i = 0; i < 5000 && engine.is_running(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(engine.has_results());

    slopkit::table::AddressTable        table;
    slopkit::ui::panels::FoundListPanel panel {engine, table};
    panel.refresh();
    panel.set_target_attached(true);

    auto* view = panel.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    REQUIRE(view->model() != nullptr);
    REQUIRE(view->model()->rowCount() == 3);

    QItemSelectionModel* selection = view->selectionModel();
    REQUIRE(selection != nullptr);
    selection->select(view->model()->index(0, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    selection->select(view->model()->index(1, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);

    std::vector<std::uint64_t> addresses;
    std::vector<std::byte>     written;
    int                        requests = 0;
    QObject::connect(&panel,
                     &slopkit::ui::panels::FoundListPanel::changeValuesRequested,
                     &panel,
                     [&](std::vector<std::uint64_t> requested, std::vector<std::byte> data)
                     {
                         addresses = std::move(requested);
                         written   = std::move(data);
                         ++requests;
                     });

    QMenu menu;
    panel.populate_row_menu(menu, 0);
    QAction* change = nullptr;
    for (QAction* action : menu.actions())
    {
        if (action->text() == QStringLiteral("Change value…"))
        {
            change = action;
        }
    }
    REQUIRE(change != nullptr);
    REQUIRE(change->isEnabled());

    QTimer::singleShot(0,
                       []
                       {
                           for (QWidget* widget : QApplication::topLevelWidgets())
                           {
                               auto* box = qobject_cast<slopkit::ui::widgets::InputBox*>(widget);
                               if (box == nullptr || !box->isVisible())
                               {
                                   continue;
                               }
                               box->line_edit()->setText(QStringLiteral("55"));
                               box->accept();
                               return;
                           }
                       });
    change->trigger();

    REQUIRE(requests == 1);
    REQUIRE(addresses.size() == 2);
    CHECK(addresses[0] != addresses[1]);
    CHECK(written == std::vector<std::byte> {std::byte {55}, std::byte {0}, std::byte {0}, std::byte {0}});
}

TEST_CASE("the found list row menu arms an access watch on a hit", "[ui]")
{
    application();

    std::vector<std::byte> bytes(8, std::byte {0});
    const std::uint32_t    value = 10;
    std::memcpy(bytes.data(), &value, sizeof(value));

    slopkit::scan::ScanEngine engine;
    slopkit::scan::ScanConfig config;
    config.value = std::int64_t {10};
    engine.first_scan(config, slopkit::scan::make_buffer_source(bytes, 0x1000));
    for (int i = 0; i < 5000 && engine.is_running(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(engine.has_results());

    slopkit::table::AddressTable        table;
    slopkit::ui::panels::FoundListPanel panel {engine, table};
    panel.refresh();
    panel.set_target_attached(true);

    std::uint64_t        requested_address = 0;
    std::size_t          requested_width   = 0;
    slopkit::debug::Kind requested_kind    = slopkit::debug::Kind::software;
    int                  requests          = 0;
    QObject::connect(&panel,
                     &slopkit::ui::panels::FoundListPanel::accessWatchRequested,
                     &panel,
                     [&](std::uint64_t address, std::size_t width, slopkit::debug::Kind kind)
                     {
                         requested_address = address;
                         requested_width   = width;
                         requested_kind    = kind;
                         ++requests;
                     });

    QMenu menu;
    panel.populate_row_menu(menu, 0);

    QMenu* watch = submenu_of(menu, QStringLiteral("Watch accesses"));
    REQUIRE(watch != nullptr);

    QAction* watch_writes   = nullptr;
    QAction* watch_accesses = nullptr;
    for (QAction* action : watch->actions())
    {
        if (action->text() == QStringLiteral("Find out what writes this address"))
        {
            watch_writes = action;
        }
        if (action->text() == QStringLiteral("Find out what accesses this address"))
        {
            watch_accesses = action;
        }
    }
    REQUIRE(watch_writes != nullptr);
    REQUIRE(watch_accesses != nullptr);
    CHECK(watch_writes->isEnabled());
    CHECK(watch_accesses->isEnabled());

    watch_writes->trigger();
    REQUIRE(requests == 1);
    CHECK(requested_address == 0x1000);
    CHECK(requested_width == 4);
    CHECK(requested_kind == slopkit::debug::Kind::hardware_write);

    watch_accesses->trigger();
    REQUIRE(requests == 2);
    CHECK(requested_kind == slopkit::debug::Kind::hardware_read_write);
}

TEST_CASE("the found list shows the static-first top of the whole result set", "[ui]")
{
    application();

    std::vector<std::byte> bytes(300 * 4, std::byte {0});
    for (std::size_t i = 0; i < 300; ++i)
    {
        const std::uint32_t value = 10;
        std::memcpy(bytes.data() + i * 4, &value, sizeof(value));
    }

    slopkit::scan::ScanEngine engine;
    slopkit::scan::ScanConfig config;
    config.value = std::int64_t {10};
    engine.first_scan(config, slopkit::scan::make_buffer_source(bytes, 0x1000));
    for (int i = 0; i < 5000 && engine.is_running(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(engine.has_results());
    REQUIRE(engine.snapshot().hit_count == 300);

    slopkit::process::ModuleInfo image;
    image.base = 0x1400;
    image.size = 0x100;
    image.kind = slopkit::process::ModuleKind::elf;
    image.name = "app";
    image.path = "/opt/app";

    slopkit::table::AddressTable        table;
    slopkit::ui::panels::FoundListPanel panel {engine, table};
    panel.set_modules({image});
    panel.refresh();

    auto* view = panel.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    auto* model = dynamic_cast<slopkit::ui::models::FoundResultsModel*>(view->model());
    REQUIRE(model != nullptr);

    CHECK(model->rowCount() == slopkit::scan::kDisplayPage);
    REQUIRE(model->hit_at(0) != nullptr);
    CHECK(model->hit_at(0)->address == 0x1400);
    CHECK(model->is_static(model->hit_at(0)->address));

    auto* header = panel.findChild<QLabel*>();
    REQUIRE(header != nullptr);
    CHECK(header->text() == QStringLiteral("Showing 256 of 300 results"));

    const std::uint64_t first = model->hit_at(0)->address;
    REQUIRE(QMetaObject::invokeMethod(
        view,
        "doubleClicked",
        Qt::DirectConnection,
        Q_ARG(QModelIndex, model->index(0, slopkit::ui::models::FoundResultsModel::address))));
    REQUIRE(table.entries().size() == 1);
    CHECK(table.entries()[0].address == first);
}

TEST_CASE("the found list tooltips an elided cell", "[ui]")
{
    application();

    std::vector<std::byte> bytes(4, std::byte {0});
    const std::uint32_t    value = 0x12345678;
    std::memcpy(bytes.data(), &value, sizeof(value));

    slopkit::scan::ScanEngine engine;
    slopkit::scan::ScanConfig config;
    config.value_type = slopkit::scan::ValueType::int32;
    config.value      = std::int64_t {0x12345678};
    config.hex        = true;
    engine.first_scan(config, slopkit::scan::make_buffer_source(bytes, 0x1000));
    for (int i = 0; i < 5000 && engine.is_running(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(engine.has_results());

    slopkit::table::AddressTable        table;
    slopkit::ui::panels::FoundListPanel panel {engine, table};
    panel.resize(400, 200);
    panel.show();
    panel.refresh();
    QCoreApplication::processEvents();

    auto* view = panel.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    auto* model = dynamic_cast<slopkit::ui::models::FoundResultsModel*>(view->model());
    REQUIRE(model != nullptr);
    REQUIRE(model->rowCount() > 0);

    const QModelIndex cell = model->index(0, slopkit::ui::models::FoundResultsModel::value);
    const QString     text = model->data(cell, Qt::DisplayRole).toString();
    REQUIRE_FALSE(text.isEmpty());

    // A column wide enough for the value stays quiet: with no tooltip shown yet,
    // the hover must not create one.
    view->setColumnWidth(slopkit::ui::models::FoundResultsModel::value, 300);
    QCoreApplication::processEvents();
    const QPoint fitted_pos = view->visualRect(cell).center();
    QHelpEvent   fitted {QEvent::ToolTip, fitted_pos, view->viewport()->mapToGlobal(fitted_pos)};
    QApplication::sendEvent(view->viewport(), &fitted);
    CHECK_FALSE(QToolTip::isVisible());
    CHECK(QToolTip::text().isEmpty());

    view->setColumnWidth(slopkit::ui::models::FoundResultsModel::value, 1);
    QCoreApplication::processEvents();
    const QPoint clipped_pos = view->visualRect(cell).center();
    QHelpEvent   clipped {QEvent::ToolTip, clipped_pos, view->viewport()->mapToGlobal(clipped_pos)};
    QApplication::sendEvent(view->viewport(), &clipped);
    CHECK(QToolTip::isVisible());
    CHECK(QToolTip::text() == text);
}

TEST_CASE("the results list Value column follows live memory for the shown rows", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    slopkit::ui::SettingsController  settings {scratch_settings_file("live_results.ini")};

    attach_app_session(worker);

    constexpr std::uint64_t     kBase  = 0x1000;
    constexpr std::size_t       kTotal = 300;
    slopkit::scan::ScanSnapshot snapshot;
    snapshot.state     = slopkit::scan::ScanState::done;
    snapshot.hit_count = kTotal;
    for (std::size_t i = 0; i < kTotal; ++i)
    {
        slopkit::scan::ScanHit hit;
        hit.address = kBase + (i * 4);
        hit.value   = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
        snapshot.hits.push_back(std::move(hit));
    }

    slopkit::scan::ScanConfig config;
    config.value_type = slopkit::scan::ValueType::int32;

    slopkit::ui::models::FoundResultsModel model;
    model.set_snapshot(std::move(snapshot), config);

    ModelLiveSurface        surface {model};
    slopkit::ui::LiveValues live {worker, target, settings};
    live.add_surface(&surface);

    const auto value_text = [&](int row)
    {
        return model.data(model.index(row, slopkit::ui::models::FoundResultsModel::value), Qt::DisplayRole).toString();
    };

    REQUIRE(model.rowCount() == static_cast<int>(slopkit::scan::kDisplayPage));
    const std::vector<slopkit::ui::LiveRequest> requests = surface.next_live_request();
    REQUIRE(requests.size() == slopkit::scan::kDisplayPage);
    const std::uint64_t last_shown = kBase + ((slopkit::scan::kDisplayPage - 1) * 4);
    for (const auto& request : requests)
    {
        CHECK(request.address >= kBase);
        CHECK(request.address <= last_shown);
    }

    CHECK(value_text(0) == QStringLiteral("1"));

    (*access.memory)[kBase]      = {std::byte {7}, std::byte {0}, std::byte {0}, std::byte {0}};
    (*access.memory)[last_shown] = {std::byte {8}, std::byte {0}, std::byte {0}, std::byte {0}};
    live.request_now();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return value_text(0) == QStringLiteral("7");
                    }));
    CHECK(value_text(static_cast<int>(slopkit::scan::kDisplayPage) - 1) == QStringLiteral("8"));

    CHECK(model.data(model.index(0, slopkit::ui::models::FoundResultsModel::address), Qt::DisplayRole)
              .toString()
              .startsWith(QStringLiteral("0x1000")));

    (*access.memory)[kBase][0] = std::byte {9};
    live.request_now();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return value_text(0) == QStringLiteral("9");
                    }));
    CHECK(model.data(model.index(0, slopkit::ui::models::FoundResultsModel::value), Qt::ForegroundRole).value<QColor>()
          == slopkit::ui::active_theme().warning);
}
