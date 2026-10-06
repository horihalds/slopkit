#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

TEST_CASE("the scanner range is pre-filled with the padded defaults", "[ui]")
{
    application();

    SECTION("independent of the target's memory map")
    {
        FakeAccess access;

        slopkit::process::RegionInfo low;
        low.start    = 0x1000;
        low.end      = 0x3000;
        low.readable = true;

        access.regions = {low};

        slopkit::process::AccessWorker   worker {access};
        slopkit::process::AttachedTarget target = fake_target();

        // No attach or map job yet: the boxes already hold the whole-address-space
        // range, independent of the target's memory map.
        slopkit::ui::panels::ScannerPanel panel {worker, target};

        auto* start = address_field(panel, "Start address");
        auto* stop  = address_field(panel, "Stop address");
        auto* combo = range_combo(panel);
        REQUIRE(start != nullptr);
        REQUIRE(stop != nullptr);
        REQUIRE(combo != nullptr);

        CHECK(start->text() == QStringLiteral("0000000000000000"));
        CHECK(stop->text() == QStringLiteral("00007FFFFFFFFFFF"));
        CHECK_FALSE(combo->isEnabled());
    }

    // Waits for the memory map, then checks the boxes hold the padded default
    // range whatever the mapped pages are.
    const auto expects_defaults = [](FakeAccess& access)
    {
        slopkit::process::AccessWorker   worker {access};
        slopkit::process::AttachedTarget target = fake_target();

        attach_app_session(worker);

        slopkit::ui::panels::ScannerPanel panel {worker, target};

        auto* start = address_field(panel, "Start address");
        auto* stop  = address_field(panel, "Stop address");
        auto* combo = range_combo(panel);
        REQUIRE(start != nullptr);
        REQUIRE(stop != nullptr);
        REQUIRE(combo != nullptr);

        REQUIRE(pump_ui(worker,
                        [&]
                        {
                            return combo->isEnabled();
                        }));
        CHECK(start->text() == QStringLiteral("0000000000000000"));
        CHECK(stop->text() == QStringLiteral("00007FFFFFFFFFFF"));
    };

    SECTION("an empty map")
    {
        FakeAccess access; // No regions reported.
        expects_defaults(access);
    }

    SECTION("a low non-readable region")
    {
        FakeAccess access;

        slopkit::process::RegionInfo guard;
        guard.start    = 0x1000;
        guard.end      = 0x2000;
        guard.readable = false;

        slopkit::process::RegionInfo readable;
        readable.start    = 0x3000;
        readable.end      = 0x6000;
        readable.readable = true;

        access.regions = {readable, guard};
        expects_defaults(access);
    }

    SECTION("a lone non-readable region")
    {
        FakeAccess access;

        slopkit::process::RegionInfo only;
        only.start     = 0x2000;
        only.end       = 0x6000;
        only.readable  = false;
        access.regions = {only};

        expects_defaults(access);
    }
}

TEST_CASE("starting a scan hands the whole address space range to the engine", "[ui]")
{
    application();

    FakeAccess                   access;
    slopkit::process::RegionInfo region;
    region.start    = 0x1000;
    region.end      = 0x3000;
    region.readable = true;
    access.regions  = {region};

    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    attach_app_session(worker);

    slopkit::ui::panels::ScannerPanel panel {worker, target};

    auto* start = address_field(panel, "Start address");
    REQUIRE(start != nullptr);

    // A first scan needs a value to search for; the range is what we assert.
    auto* value = address_field(panel, "Value");
    REQUIRE(value != nullptr);
    value->setText(QStringLiteral("10"));

    QPushButton* scan_button = nullptr;
    for (auto* button : panel.findChildren<QPushButton*>())
    {
        if (button->text() == QStringLiteral("First Scan"))
        {
            scan_button = button;
            break;
        }
    }
    REQUIRE(scan_button != nullptr);

    // Wait until the map and the session handoff land: a refresh then enables
    // scanning.
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        panel.refresh();
                        return scan_button->isEnabled();
                    }));
    scan_button->click();

    const auto config = panel.engine().config();
    CHECK(config.filter.start == 0);
    CHECK(config.filter.stop == slopkit::scan::kMaxUserAddress);
}

TEST_CASE("a rejected scan is reported through the log, not a panel line", "[ui]")
{
    application();

    FakeAccess                   access;
    slopkit::process::RegionInfo region;
    region.start    = 0x1000;
    region.end      = 0x3000;
    region.readable = true;
    access.regions  = {region};

    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    attach_app_session(worker);

    slopkit::ui::panels::ScannerPanel panel {worker, target};

    auto* value = address_field(panel, "Value");
    REQUIRE(value != nullptr);

    // Capture every record while the panel is exercised.
    std::vector<slopkit::log::Record> records;
    const auto                        sink = slopkit::log::Logger::instance().add_sink(
        [&records](const slopkit::log::Record& record)
        {
            records.push_back(record);
        });
    slopkit::log::Logger::instance().set_minimum_level(slopkit::log::Level::info);

    // A first scan needs a value to search for; the empty box is rejected.
    auto* scan_button = button_labelled(panel, QStringLiteral("First Scan"));
    REQUIRE(scan_button != nullptr);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        panel.refresh();
                        return scan_button->isEnabled();
                    }));
    value->clear();
    scan_button->click();

    slopkit::log::Logger::instance().remove_sink(sink);

    bool saw_reject = false;
    for (const auto& record : records)
    {
        if (record.category == "scan" && record.level == slopkit::log::Level::warning
            && record.message.starts_with("Value: "))
        {
            saw_reject = true;
        }
    }
    CHECK(saw_reject);

    // The removed idle hint is nowhere in the panel in any state.
    for (auto* label : panel.findChildren<QLabel*>())
    {
        CHECK(label->text() != QStringLiteral("Select a process to enable scanning."));
    }
}

TEST_CASE("New Scan clears the results and returns the panel to its pre-scan state", "[ui]")
{
    application();

    FakeAccess                   access;
    slopkit::process::RegionInfo region;
    region.start    = 0x1000;
    region.end      = 0x3000;
    region.readable = true;
    region.writable = true;
    access.regions  = {region};

    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    attach_app_session(worker);

    slopkit::ui::panels::ScannerPanel panel {worker, target};

    auto* value = address_field(panel, "Value");
    REQUIRE(value != nullptr);
    value->setText(QStringLiteral("10"));

    auto* scan_button = button_labelled(panel, QStringLiteral("First Scan"));
    auto* undo_button = button_labelled(panel, QStringLiteral("Undo Scan"));
    auto* next_button = button_labelled(panel, QStringLiteral("Next Scan"));
    REQUIRE(scan_button != nullptr);
    REQUIRE(undo_button != nullptr);
    REQUIRE(next_button != nullptr);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        panel.refresh();
                        return scan_button->isEnabled();
                    }));

    // The first scan runs to completion and leaves a result set behind.
    scan_button->click();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        panel.refresh();
                        return panel.engine().has_results() && !panel.engine().is_running();
                    }));
    panel.refresh();
    REQUIRE(scan_button->text() == QStringLiteral("New Scan"));
    CHECK_FALSE(undo_button->isEnabled());

    // New Scan drops the result set instead of starting another scan.
    scan_button->click();
    panel.refresh();

    CHECK_FALSE(panel.engine().has_results());
    CHECK(scan_button->text() == QStringLiteral("First Scan"));
    CHECK_FALSE(undo_button->isEnabled());
    CHECK_FALSE(next_button->isEnabled());
    CHECK(panel.progress_percent() == 0);

    // The found list over the same engine is empty again.
    slopkit::table::AddressTable        addresses;
    slopkit::ui::panels::FoundListPanel found_list {panel.engine(), addresses};
    found_list.refresh();
    auto* view = found_list.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    REQUIRE(view->model() != nullptr);
    CHECK(view->model()->rowCount() == 0);
}

TEST_CASE("Undo Scan is gated on a refinement being available", "[ui]")
{
    application();

    FakeAccess                   access;
    slopkit::process::RegionInfo region;
    region.start    = 0x1000;
    region.end      = 0x3000;
    region.readable = true;
    region.writable = true;
    access.regions  = {region};

    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    attach_app_session(worker);

    slopkit::ui::panels::ScannerPanel panel {worker, target};

    auto* value = address_field(panel, "Value");
    REQUIRE(value != nullptr);
    value->setText(QStringLiteral("10"));

    auto* scan_button = button_labelled(panel, QStringLiteral("First Scan"));
    auto* undo_button = button_labelled(panel, QStringLiteral("Undo Scan"));
    auto* next_button = button_labelled(panel, QStringLiteral("Next Scan"));
    REQUIRE(scan_button != nullptr);
    REQUIRE(undo_button != nullptr);
    REQUIRE(next_button != nullptr);

    // Wait until the handed-over session has arrived and scanning is possible.
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        panel.refresh();
                        return scan_button->isEnabled();
                    }));

    // A first scan leaves a result set but no history, so Undo Scan stays off.
    scan_button->click();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        panel.refresh();
                        return panel.engine().has_results() && !panel.engine().is_running();
                    }));
    panel.refresh();
    CHECK_FALSE(panel.engine().can_undo());
    CHECK_FALSE(undo_button->isEnabled());
    REQUIRE(next_button->isEnabled());

    // A refinement records the previous set, which enables Undo Scan.
    next_button->click();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        panel.refresh();
                        return panel.engine().can_undo() && !panel.engine().is_running();
                    }));
    panel.refresh();
    CHECK(undo_button->isEnabled());

    // Undoing the only refinement exhausts the history and disables the button.
    undo_button->click();
    panel.refresh();
    CHECK_FALSE(panel.engine().can_undo());
    CHECK_FALSE(undo_button->isEnabled());
    CHECK(panel.engine().has_results());
}

TEST_CASE("the scanner value box focuses and selects all its text", "[ui]")
{
    application();

    FakeAccess                   access;
    slopkit::process::RegionInfo region;
    region.start    = 0x1000;
    region.end      = 0x3000;
    region.readable = true;
    access.regions  = {region};

    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    attach_app_session(worker);

    slopkit::ui::panels::ScannerPanel panel {worker, target};
    panel.show();

    auto* value = address_field(panel, "Value");
    REQUIRE(value != nullptr);
    value->setText(QStringLiteral("1234"));

    panel.focus_value_input();

    CHECK(panel.focusWidget() == value);
    CHECK(value->selectedText() == QStringLiteral("1234"));
}

TEST_CASE("Enter in the scanner value box runs the first scan then a next scan", "[ui]")
{
    application();

    FakeAccess                   access;
    slopkit::process::RegionInfo region;
    region.start    = 0x1000;
    region.end      = 0x3000;
    region.readable = true;
    region.writable = true;
    access.regions  = {region};

    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    attach_app_session(worker);

    slopkit::ui::panels::ScannerPanel panel {worker, target};

    auto* value = address_field(panel, "Value");
    REQUIRE(value != nullptr);
    value->setText(QStringLiteral("10"));

    auto* scan_button = button_labelled(panel, QStringLiteral("First Scan"));
    REQUIRE(scan_button != nullptr);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        panel.refresh();
                        return scan_button->isEnabled();
                    }));

    std::vector<slopkit::log::Record> records;
    const LevelGuard                  level_guard;
    const SinkGuard                   sink_guard(
        [&records](const slopkit::log::Record& record)
        {
            records.push_back(record);
        });
    slopkit::log::Logger::instance().set_minimum_level(slopkit::log::Level::debug);

    const auto press_enter = [](QLineEdit* edit)
    {
        QKeyEvent enter {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
        QCoreApplication::sendEvent(edit, &enter);
    };

    // Enter starts the very first scan, so the button turns into New Scan.
    press_enter(value);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        panel.refresh();
                        return panel.engine().has_results() && !panel.engine().is_running();
                    }));
    panel.refresh();
    CHECK(scan_button->text() == QStringLiteral("New Scan"));

    // Enter again refines the existing result set instead of resetting it: the
    // engine publishes a fresh result handle once the refinement finishes.
    const auto before = panel.engine().snapshot().result_hits;
    value->setText(QStringLiteral("20"));
    press_enter(value);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        panel.refresh();
                        return panel.engine().snapshot().result_hits != before && !panel.engine().is_running();
                    }));
    CHECK(panel.engine().has_results());

    int first_records = 0;
    int next_records  = 0;
    for (const auto& record : records)
    {
        if (record.category == "ui" && record.message == "First Scan")
        {
            ++first_records;
        }
        if (record.category == "ui" && record.message == "Next Scan")
        {
            ++next_records;
        }
    }
    CHECK(first_records == 1);
    CHECK(next_records == 1);
}

TEST_CASE("Enter in the scanner value box rejects an empty value through the log", "[ui]")
{
    application();

    FakeAccess                   access;
    slopkit::process::RegionInfo region;
    region.start    = 0x1000;
    region.end      = 0x3000;
    region.readable = true;
    access.regions  = {region};

    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    attach_app_session(worker);

    slopkit::ui::panels::ScannerPanel panel {worker, target};

    auto* value = address_field(panel, "Value");
    REQUIRE(value != nullptr);
    auto* scan_button = button_labelled(panel, QStringLiteral("First Scan"));
    REQUIRE(scan_button != nullptr);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        panel.refresh();
                        return scan_button->isEnabled();
                    }));

    std::vector<slopkit::log::Record> records;
    const LevelGuard                  level_guard;
    const SinkGuard                   sink_guard(
        [&records](const slopkit::log::Record& record)
        {
            records.push_back(record);
        });
    slopkit::log::Logger::instance().set_minimum_level(slopkit::log::Level::info);

    value->clear();
    QKeyEvent enter {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QCoreApplication::sendEvent(value, &enter);

    bool saw_reject = false;
    for (const auto& record : records)
    {
        if (record.category == "scan" && record.level == slopkit::log::Level::warning
            && record.message.starts_with("Value: "))
        {
            saw_reject = true;
        }
    }
    CHECK(saw_reject);
    CHECK_FALSE(panel.engine().has_results());
}

TEST_CASE("the scanner value box is skipped when its scan type needs no value", "[ui]")
{
    application();

    FakeAccess                   access;
    slopkit::process::RegionInfo region;
    region.start    = 0x1000;
    region.end      = 0x3000;
    region.readable = true;
    access.regions  = {region};

    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    attach_app_session(worker);

    slopkit::ui::panels::ScannerPanel panel {worker, target};
    panel.show();

    auto* value = address_field(panel, "Value");
    auto* start = address_field(panel, "Start address");
    REQUIRE(value != nullptr);
    REQUIRE(start != nullptr);

    // "Unknown initial value" needs no value, so the field is disabled.
    for (auto* combo : panel.findChildren<QComboBox*>())
    {
        if (combo->currentText() == QStringLiteral("Exact Value"))
        {
            combo->setCurrentText(QStringLiteral("Unknown initial value"));
        }
    }
    CHECK_FALSE(value->isEnabled());

    start->setFocus();
    panel.focus_value_input();
    CHECK(panel.focusWidget() != value);
}

TEST_CASE("the scan range dropdown lists file-backed modules and narrows the range", "[ui]")
{
    application();

    FakeAccess access;

    slopkit::process::RegionInfo region;
    region.start    = 0x1000;
    region.end      = 0x3000;
    region.readable = true;
    access.regions  = {region};

    slopkit::process::ModuleInfo high;
    high.base = 0x2000;
    high.size = 0x100;
    high.kind = slopkit::process::ModuleKind::elf;
    high.name = "high";
    high.path = "/opt/high";

    slopkit::process::ModuleInfo low;
    low.base = 0x1000;
    low.size = 0x800;
    low.kind = slopkit::process::ModuleKind::elf;
    low.name = "low";
    low.path = "/opt/low";

    slopkit::process::ModuleInfo anon;
    anon.base = 0x4000;
    anon.size = 0x1000;
    anon.kind = slopkit::process::ModuleKind::anonymous;
    anon.name = "[anon]";

    // Deliberately out of base order to prove the dropdown sorts.
    access.modules = {high, anon, low};

    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    attach_app_session(worker);

    slopkit::ui::panels::ScannerPanel panel {worker, target};

    auto* start = address_field(panel, "Start address");
    auto* stop  = address_field(panel, "Stop address");
    auto* combo = range_combo(panel);
    REQUIRE(start != nullptr);
    REQUIRE(stop != nullptr);
    REQUIRE(combo != nullptr);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return combo->isEnabled();
                    }));

    // Item 0 is the whole process, then the file-backed modules by base; the
    // anonymous mapping stays out of the list.
    REQUIRE(combo->count() == 3);
    CHECK(combo->itemText(0) == QStringLiteral("All memory"));
    CHECK(combo->itemText(1) == QStringLiteral("low"));
    CHECK(combo->itemText(2) == QStringLiteral("high"));
    for (int i = 0; i < combo->count(); ++i)
    {
        CHECK_FALSE(combo->itemText(i).contains(QStringLiteral("0x")));
    }
    // The range moved into the tooltips instead of the entry text.
    CHECK(combo->itemData(0, Qt::ToolTipRole).toString() == QStringLiteral("0000000000000000-00007FFFFFFFFFFF"));
    CHECK(combo->itemData(1, Qt::ToolTipRole).toString() == QStringLiteral("/opt/low"));

    // Selecting a module narrows the range to its base and size.
    combo->setCurrentIndex(1);
    CHECK(start->text() == QStringLiteral("0000000000001000"));
    CHECK(stop->text() == QStringLiteral("0000000000001800"));

    combo->setCurrentIndex(2);
    CHECK(start->text() == QStringLiteral("0000000000002000"));
    CHECK(stop->text() == QStringLiteral("0000000000002100"));

    // Back to the whole process.
    combo->setCurrentIndex(0);
    CHECK(start->text() == QStringLiteral("0000000000000000"));
    CHECK(stop->text() == QStringLiteral("00007FFFFFFFFFFF"));
}

TEST_CASE("the scan range dropdown pins the main image after All memory", "[ui]")
{
    application();

    FakeAccess access;

    slopkit::process::RegionInfo region;
    region.start    = 0x1000;
    region.end      = 0x9000;
    region.readable = true;
    access.regions  = {region};

    slopkit::process::ModuleInfo low;
    low.base = 0x1000;
    low.size = 0x800;
    low.kind = slopkit::process::ModuleKind::elf;
    low.name = "low";
    low.path = "/opt/low";

    slopkit::process::ModuleInfo game;
    game.base    = 0x5000;
    game.size    = 0x1000;
    game.kind    = slopkit::process::ModuleKind::elf;
    game.name    = "game";
    game.path    = "/opt/game";
    game.is_main = true;

    slopkit::process::ModuleInfo lib;
    lib.base = 0x8000;
    lib.size = 0x100;
    lib.kind = slopkit::process::ModuleKind::elf;
    lib.name = "lib";
    lib.path = "/opt/lib";

    slopkit::process::ModuleInfo anon;
    anon.base = 0x3000;
    anon.size = 0x1000;
    anon.kind = slopkit::process::ModuleKind::anonymous;
    anon.name = "[anon]";

    // Deliberately out of base order; the flagged image is not the lowest-based.
    access.modules = {low, anon, game, lib};

    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    attach_app_session(worker);

    slopkit::ui::panels::ScannerPanel panel {worker, target};

    auto* start = address_field(panel, "Start address");
    auto* stop  = address_field(panel, "Stop address");
    auto* combo = range_combo(panel);
    REQUIRE(start != nullptr);
    REQUIRE(stop != nullptr);
    REQUIRE(combo != nullptr);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return combo->isEnabled();
                    }));

    // Item 0 is the whole process, then the flagged main image, then the
    // remaining file-backed modules by base; the anonymous mapping stays out.
    REQUIRE(combo->count() == 4);
    CHECK(combo->itemText(0) == QStringLiteral("All memory"));
    CHECK(combo->itemText(1) == QStringLiteral("game"));
    CHECK(combo->itemText(2) == QStringLiteral("low"));
    CHECK(combo->itemText(3) == QStringLiteral("lib"));
    CHECK(combo->itemData(1, Qt::ToolTipRole).toString() == QStringLiteral("/opt/game"));

    // Each row still narrows Start/Stop to that image's span.
    combo->setCurrentIndex(1);
    CHECK(start->text() == QStringLiteral("0000000000005000"));
    CHECK(stop->text() == QStringLiteral("0000000000006000"));

    combo->setCurrentIndex(2);
    CHECK(start->text() == QStringLiteral("0000000000001000"));
    CHECK(stop->text() == QStringLiteral("0000000000001800"));
}

TEST_CASE("a memory map applied to the scanner panel reaches the found list", "[ui]")
{
    application();

    FakeAccess access;

    slopkit::process::RegionInfo region;
    region.start    = 0x1000;
    region.end      = 0x3000;
    region.readable = true;
    access.regions  = {region};

    slopkit::process::ModuleInfo image;
    image.base     = 0x1000;
    image.size     = 0x2000;
    image.kind     = slopkit::process::ModuleKind::elf;
    image.name     = "app";
    image.path     = "/opt/app";
    access.modules = {image};

    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    attach_app_session(worker);

    slopkit::ui::panels::ScannerPanel panel {worker, target};

    slopkit::table::AddressTable        addresses;
    slopkit::ui::panels::FoundListPanel found_list {panel.engine(), addresses};
    QObject::connect(&panel,
                     &slopkit::ui::panels::ScannerPanel::memoryMapApplied,
                     &found_list,
                     &slopkit::ui::panels::FoundListPanel::set_modules);

    auto* combo = range_combo(panel);
    REQUIRE(combo != nullptr);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return combo->isEnabled();
                    }));

    auto* view = found_list.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    auto* model = dynamic_cast<slopkit::ui::models::FoundResultsModel*>(view->model());
    REQUIRE(model != nullptr);

    // The module image span arrived through the signal: inside is static, the
    // half-open end and an unrelated address are not.
    CHECK(model->is_static(0x1000));
    CHECK(model->is_static(0x2FFF));
    CHECK_FALSE(model->is_static(0x3000));
    CHECK_FALSE(model->is_static(0x5000000));
}

TEST_CASE("a long module list scrolls inside the scan-range dropdown", "[ui]")
{
    application();

    slopkit::ui::widgets::ScrollingComboBox combo;
    combo.resize(320, 30);
    for (int i = 0; i < 40; ++i)
    {
        combo.addItem(
            QStringLiteral("libmodule%1.so.6  0x7F0%2-0x7F1%2").arg(i).arg(i * 0x1000, 8, 16, QLatin1Char('0')));
    }
    combo.show();
    QCoreApplication::processEvents();

    combo.showPopup();
    QCoreApplication::processEvents();

    QAbstractItemView* view = combo.view();
    REQUIRE(view != nullptr);
    CHECK(view->window() != &combo);

    const int row_height = view->sizeHintForRow(0);
    REQUIRE(row_height > 0);

    // The popup shows at most the visible rows and scrolls the rest instead of
    // growing over the whole screen.
    CHECK(view->height() <= row_height * combo.maxVisibleItems());
    CHECK(view->verticalScrollBar()->maximum() > 0);

    combo.hidePopup();
}

TEST_CASE("manual edits to the scan range survive refresh cycles", "[ui]")
{
    application();

    FakeAccess                   access;
    slopkit::process::RegionInfo region;
    region.start    = 0x1000;
    region.end      = 0x3000;
    region.readable = true;
    access.regions  = {region};

    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    attach_app_session(worker);

    slopkit::ui::panels::ScannerPanel panel {worker, target};

    auto* start = address_field(panel, "Start address");
    auto* combo = range_combo(panel);
    REQUIRE(start != nullptr);
    REQUIRE(combo != nullptr);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return combo->isEnabled();
                    }));
    CHECK(start->text() == QStringLiteral("0000000000000000"));

    // A hand-typed value must not be rewritten by later ticks.
    start->setText(QStringLiteral("0x2000"));
    for (int tick = 0; tick < 5; ++tick)
    {
        panel.refresh();
        worker.drain();
        QCoreApplication::processEvents();
    }
    CHECK(start->text() == QStringLiteral("0x2000"));
}

TEST_CASE("detaching resets the scan range and re-attaching repopulates it", "[ui]")
{
    application();

    FakeAccess                   access;
    slopkit::process::RegionInfo region;
    region.start    = 0x1000;
    region.end      = 0x3000;
    region.readable = true;
    access.regions  = {region};

    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    attach_app_session(worker);

    slopkit::ui::panels::ScannerPanel panel {worker, target};

    auto* start = address_field(panel, "Start address");
    auto* stop  = address_field(panel, "Stop address");
    auto* combo = range_combo(panel);
    REQUIRE(start != nullptr);
    REQUIRE(stop != nullptr);
    REQUIRE(combo != nullptr);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return combo->isEnabled();
                    }));
    CHECK(start->text() == QStringLiteral("0000000000000000"));

    // Detach: the boxes return to the padded defaults and the dropdown resets
    // to a disabled entry.
    target.clear();
    panel.refresh();
    CHECK(start->text() == QStringLiteral("0000000000000000"));
    CHECK(stop->text() == QStringLiteral("00007FFFFFFFFFFF"));
    CHECK_FALSE(combo->isEnabled());
    REQUIRE(combo->count() == 1);
    CHECK(combo->itemText(0) == QStringLiteral("All memory"));

    // Re-attach: the map is requested again and the defaults are restored.
    target = fake_target();
    panel.refresh();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return combo->isEnabled();
                    }));
    CHECK(start->text() == QStringLiteral("0000000000000000"));
    CHECK(stop->text() == QStringLiteral("00007FFFFFFFFFFF"));
}

TEST_CASE("a stale memory map result does not overwrite the range", "[ui]")
{
    application();

    FakeAccess                   access;
    slopkit::process::RegionInfo region;
    region.start    = 0x1000;
    region.end      = 0x3000;
    region.readable = true;
    access.regions  = {region};

    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    // Build the panel before the app-wide attach, so the handoff drains without
    // a map request: the map is only asked for on an explicit refresh below.
    slopkit::ui::panels::ScannerPanel panel {worker, target};

    auto* start = address_field(panel, "Start address");
    auto* combo = range_combo(panel);
    REQUIRE(start != nullptr);
    REQUIRE(combo != nullptr);

    attach_app_session(worker);
    panel.refresh(); // Submits the map job for pid 42 (shows "Loading…").

    // Let the job finish and queue its completion, then change the target
    // before the completion is drained: it must be dropped.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    target.pid = 99;
    worker.drain();
    QCoreApplication::processEvents();

    CHECK(start->text() == QStringLiteral("0000000000000000"));
    CHECK_FALSE(combo->isEnabled());
    REQUIRE(combo->count() == 1);
    CHECK(combo->itemText(0) == QStringLiteral("Loading…"));
}

TEST_CASE("the scanner resolves the main module entry point", "[ui]")
{
    application();

    const auto address_for = [](std::uint64_t low_entry, bool flag_high = false)
    {
        FakeAccess access;

        slopkit::process::ModuleInfo high;
        high.base    = 0x2000;
        high.size    = 0x100;
        high.entry   = 0x2100;
        high.kind    = slopkit::process::ModuleKind::elf;
        high.name    = "high";
        high.path    = "/opt/high";
        high.is_main = flag_high;

        slopkit::process::ModuleInfo low;
        low.base  = 0x1000;
        low.size  = 0x800;
        low.entry = low_entry;
        low.kind  = slopkit::process::ModuleKind::elf;
        low.name  = "low";
        low.path  = "/opt/low";

        slopkit::process::ModuleInfo anon;
        anon.base = 0x4000;
        anon.size = 0x1000;
        anon.kind = slopkit::process::ModuleKind::anonymous;
        anon.name = "[anon]";

        // Deliberately out of base order to prove the resolution sorts.
        access.modules = {high, anon, low};

        slopkit::process::AccessWorker   worker {access};
        slopkit::process::AttachedTarget target = fake_target();
        attach_app_session(worker);

        slopkit::ui::panels::ScannerPanel panel {worker, target};
        pump_ui(worker,
                [&]
                {
                    return panel.main_module_address() != 0;
                });
        return panel.main_module_address();
    };

    // A known entry point wins over the module base...
    CHECK(address_for(0x1040) == 0x1040);

    // ...and a zero entry falls back to the lowest-base module.
    CHECK(address_for(0) == 0x1000);

    // A flagged image wins even when another file-backed module has a lower
    // base, and its entry point (not the base) is reported.
    CHECK(address_for(0x1040, true) == 0x2100);

    // Without a memory map there is no address at all.
    FakeAccess                        no_map_access;
    slopkit::process::AccessWorker    no_map_worker {no_map_access};
    slopkit::process::AttachedTarget  invalid_target;
    slopkit::ui::panels::ScannerPanel no_map_panel {no_map_worker, invalid_target};
    CHECK(no_map_panel.main_module_address() == 0);
}
