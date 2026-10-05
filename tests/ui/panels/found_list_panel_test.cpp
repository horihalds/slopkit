#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

TEST_CASE("the found list keeps one result line", "[ui]")
{
    application();

    // A fresh panel already shows the pre-scan line, and it only ever has the one
    // label, so nothing can appear or disappear and shift the table.
    slopkit::scan::ScanEngine           idle_engine;
    slopkit::table::AddressTable        idle_table;
    slopkit::ui::panels::FoundListPanel idle_panel {idle_engine, idle_table};
    auto*                               idle_header = idle_panel.findChild<QLabel*>();
    REQUIRE(idle_header != nullptr);
    CHECK(idle_header->text() == QStringLiteral("Showing 0 of 0 results"));
    CHECK(idle_panel.findChildren<QLabel*>().size() == 1);

    // A capped page keeps the same line and appends the cap suffix instead of
    // adding a second status row.
    std::vector<std::byte> bytes(12, std::byte {0});
    for (std::size_t i = 0; i < 3; ++i)
    {
        const std::uint32_t value = 10;
        std::memcpy(bytes.data() + i * 4, &value, sizeof(value));
    }

    slopkit::scan::ScanEngine engine;
    engine.set_max_stored_hits(2);
    slopkit::scan::ScanConfig config;
    config.value = std::int64_t {10};
    engine.first_scan(config, slopkit::scan::make_buffer_source(bytes, 0x1000));
    for (int i = 0; i < 5000 && engine.is_running(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(engine.has_results());
    const auto snapshot = engine.snapshot();
    REQUIRE(snapshot.hits.size() == 2);
    REQUIRE(snapshot.hit_count == 3);
    REQUIRE(snapshot.truncated);

    slopkit::table::AddressTable        table;
    slopkit::ui::panels::FoundListPanel panel {engine, table};
    panel.refresh();

    auto* header = panel.findChild<QLabel*>();
    REQUIRE(header != nullptr);
    CHECK(header->text() == QStringLiteral("Showing 2 of 3 results (result cap reached)"));
    CHECK(panel.findChildren<QLabel*>().size() == 1);
}

TEST_CASE("the found list stays empty until the scan finishes", "[ui]")
{
    application();

    // 300 matches at 4-byte spacing.
    auto bytes = std::make_shared<std::vector<std::byte>>(300 * 4, std::byte {0});
    for (std::size_t i = 0; i < 300; ++i)
    {
        const std::uint32_t value = 10;
        std::memcpy(bytes->data() + i * 4, &value, sizeof(value));
    }

    // Hold the scan's read so its running state stays observable.
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

    // While the scan runs the list is empty and the header reports progress.
    panel.refresh();
    CHECK(model->rowCount() == 0);
    CHECK(header->text().startsWith(QStringLiteral("Scanning... ")));
    CHECK(view->selectionModel()->selectedRows().isEmpty());

    // Releasing the read lets the scan finish; the rows appear with it.
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

    // 300 matches at 4-byte spacing.
    auto bytes = std::make_shared<std::vector<std::byte>>(300 * 4, std::byte {0});
    for (std::size_t i = 0; i < 300; ++i)
    {
        const std::uint32_t value = 10;
        std::memcpy(bytes->data() + i * 4, &value, sizeof(value));
    }

    // Hold the refinement's read so the cancel lands while it runs.
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

    // Refine while its read is held, then cancel before it finishes.
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

    // The previous result set is back on screen.
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

    // An empty model still builds the row menu, so a stale row cannot break it.
    QMenu menu;
    panel.populate_row_menu(menu, 0);

    CHECK(action_texts(menu.actions())
          == QList<QString> {QStringLiteral("Add to address table"), QStringLiteral("Copy")});

    QMenu* copy = nullptr;
    for (QAction* action : menu.actions())
    {
        if (action->menu() != nullptr)
        {
            copy = action->menu();
        }
    }
    REQUIRE(copy != nullptr);
    CHECK(action_texts(copy->actions())
          == QList<QString> {QStringLiteral("Address (module + RVA)"),
                             QStringLiteral("Address (absolute)"),
                             QStringLiteral("Address + value")});

    // Triggering the entries with no hit behind them is a no-op, not a crash.
    menu.actions().front()->trigger();
    copy->actions().front()->trigger();
}

TEST_CASE("the found list shows the static-first top of the whole result set", "[ui]")
{
    application();

    // 300 matches at 4-byte spacing; the first page holds no static hit.
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
    image.base = 0x1400; // Covers the hits from index 256 upwards.
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

    // The late static hit leads the one-page window, and the line reports the
    // rows on screen against every match of the scan.
    CHECK(model->rowCount() == slopkit::scan::kDisplayPage);
    REQUIRE(model->hit_at(0) != nullptr);
    CHECK(model->hit_at(0)->address == 0x1400);
    CHECK(model->is_static(model->hit_at(0)->address));

    auto* header = panel.findChild<QLabel*>();
    REQUIRE(header != nullptr);
    CHECK(header->text() == QStringLiteral("Showing 256 of 300 results"));

    // Double-clicking adds exactly the hit the row shows.
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

    // One int32 hit whose hex rendering is wider than a collapsed column.
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

    // A column narrower than the value reveals the whole text on hover.
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

    // More hits than the display page, so only the shown window may be read.
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

    // Only the displayed page is requested, and it is the lowest addresses.
    REQUIRE(model.rowCount() == static_cast<int>(slopkit::scan::kDisplayPage));
    const std::vector<slopkit::ui::LiveRequest> requests = surface.next_live_request();
    REQUIRE(requests.size() == slopkit::scan::kDisplayPage);
    const std::uint64_t last_shown = kBase + ((slopkit::scan::kDisplayPage - 1) * 4);
    for (const auto& request : requests)
    {
        CHECK(request.address >= kBase);
        CHECK(request.address <= last_shown);
    }

    // The scan values show until the first reading lands.
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

    // The row order is the scan's, unchanged by the live readings.
    CHECK(model.data(model.index(0, slopkit::ui::models::FoundResultsModel::address), Qt::DisplayRole)
              .toString()
              .startsWith(QStringLiteral("0x1000")));

    // A changed reading is flagged with the theme's warning colour.
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
