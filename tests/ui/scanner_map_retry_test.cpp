#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

namespace
{
    using namespace std::chrono_literals;

    using slopkit::log::Level;
    using slopkit::process::AccessWorker;
    using slopkit::process::AttachedTarget;
    using slopkit::process::ModuleInfo;
    using slopkit::ui::panels::ScannerPanel;

    std::vector<ModuleInfo> retry_modules()
    {
        ModuleInfo main_image = module_image("game.exe", 0x140000000, 0x1000);
        main_image.is_main    = true;
        return {std::move(main_image), module_image("kernel32.dll", 0x7B600000, 0x800)};
    }

    constexpr std::string_view kMapWarning  = "could not load the memory map";
    constexpr std::string_view kMapRetryLog = "retrying the memory map";

    // Counts the scanner's own memory-map records: the first failure of a target
    // warns, later attempts are debug, so a working retry loop keeps the warning
    // count at one. The sink runs on the thread that applies the map, so atomic
    // counters keep the worker's records from racing the assertions.
    struct MapLogCounts
    {
        std::atomic<int> warnings {0};
        std::atomic<int> retries {0};
    };

    SinkGuard map_log_sink(MapLogCounts& counts)
    {
        return SinkGuard(
            [&counts](const slopkit::log::Record& record)
            {
                if (record.category != "scan")
                {
                    return;
                }
                if (record.level == Level::warning && record.message.starts_with(kMapWarning))
                {
                    ++counts.warnings;
                }
                if (record.level == Level::debug && record.message.starts_with(kMapRetryLog))
                {
                    ++counts.retries;
                }
            });
    }

    // One attach calls `modules()` for its read check, so the first map fetch is
    // the second listing of the session.
    constexpr int kListingBeforeFirstFetch = 2;
}

TEST_CASE("the scanner retries a failed memory map until the module list arrives", "[ui]")
{
    application();

    FakeAccess access;
    access.modules         = retry_modules();
    access.modules_failing = std::make_shared<std::atomic<bool>>(true);
    access.modules_calls   = std::make_shared<std::atomic<int>>(0);

    AccessWorker   worker {access};
    AttachedTarget target = fake_target();
    attach_app_session(worker);

    ScannerPanel panel {worker, target};

    auto* combo = range_combo(panel);
    REQUIRE(combo != nullptr);

    MapLogCounts     counts;
    const LevelGuard level_guard;
    const SinkGuard  sink_guard = map_log_sink(counts);
    slopkit::log::Logger::instance().set_minimum_level(slopkit::log::Level::info);

    std::vector<std::size_t> published;
    QObject::connect(&panel,
                     &ScannerPanel::memoryMapApplied,
                     [&published](std::vector<ModuleInfo> modules)
                     {
                         published.push_back(modules.size());
                     });

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        panel.refresh();
                        return counts.warnings.load() >= 1;
                    }));

    // The failed fetch published nothing and left the placeholder up.
    CHECK(published.empty());
    CHECK_FALSE(combo->isEnabled());
    CHECK(access.modules_calls->load() == kListingBeforeFirstFetch);

    access.modules_failing->store(false);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        panel.refresh();
                        return combo->isEnabled();
                    }));

    CHECK(combo->count() == 3);
    CHECK(combo->itemText(0) == QStringLiteral("All memory"));
    CHECK(combo->itemText(1) == QStringLiteral("game.exe"));
    CHECK(combo->itemText(2) == QStringLiteral("kernel32.dll"));
    CHECK(panel.main_module_address() == 0x140000000U);

    // Only the successful fetch reached a surface.
    REQUIRE(published.size() == 1);
    CHECK(published[0] == 2);
}

TEST_CASE("a failed memory map fetch leaves the applied map in place", "[ui]")
{
    application();

    FakeAccess access;
    access.modules         = retry_modules();
    access.modules_failing = std::make_shared<std::atomic<bool>>(false);
    access.modules_calls   = std::make_shared<std::atomic<int>>(0);

    AccessWorker   worker {access};
    AttachedTarget target = fake_target();
    attach_app_session(worker);

    ScannerPanel panel {worker, target};

    auto* combo = range_combo(panel);
    auto* start = address_field(panel, "Start address");
    auto* stop  = address_field(panel, "Stop address");
    REQUIRE(combo != nullptr);
    REQUIRE(start != nullptr);
    REQUIRE(stop != nullptr);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return combo->isEnabled();
                    }));
    REQUIRE(panel.main_module_address() == 0x140000000U);

    combo->setCurrentIndex(1);
    const QString scoped_start = start->text();
    const QString scoped_stop  = stop->text();
    REQUIRE(combo->currentText() == QStringLiteral("game.exe"));

    MapLogCounts     counts;
    const LevelGuard level_guard;
    const SinkGuard  sink_guard = map_log_sink(counts);
    slopkit::log::Logger::instance().set_minimum_level(slopkit::log::Level::info);

    // A switch to a target whose module listing fails must not touch the map.
    access.modules_failing->store(true);
    target.pid = 99;

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        panel.refresh();
                        return counts.warnings.load() >= 1;
                    }));

    CHECK(combo->isEnabled());
    CHECK(combo->count() == 3);
    CHECK(combo->currentText() == QStringLiteral("game.exe"));
    CHECK(start->text() == scoped_start);
    CHECK(stop->text() == scoped_stop);
    CHECK(panel.main_module_address() == 0x140000000U);
}

TEST_CASE("memory map retries are rate-limited and reported once", "[ui]")
{
    application();

    FakeAccess access;
    access.modules         = retry_modules();
    access.modules_failing = std::make_shared<std::atomic<bool>>(true);
    access.modules_calls   = std::make_shared<std::atomic<int>>(0);

    AccessWorker   worker {access};
    AttachedTarget target = fake_target();
    attach_app_session(worker);

    ScannerPanel panel {worker, target};

    auto* combo = range_combo(panel);
    REQUIRE(combo != nullptr);

    MapLogCounts     counts;
    const LevelGuard level_guard;
    const SinkGuard  sink_guard = map_log_sink(counts);
    slopkit::log::Logger::instance().set_minimum_level(slopkit::log::Level::debug);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        panel.refresh();
                        return counts.warnings.load() >= 1;
                    }));
    CHECK_FALSE(combo->isEnabled());
    CHECK(access.modules_calls->load() == kListingBeforeFirstFetch);

    // A burst of ticks inside the backoff window submits no further job.
    for (int tick = 0; tick < 10; ++tick)
    {
        panel.refresh();
        worker.drain();
        QCoreApplication::processEvents();
    }
    CHECK(access.modules_calls->load() == kListingBeforeFirstFetch);
    CHECK_FALSE(combo->isEnabled());
    CHECK(counts.warnings.load() == 1);
    CHECK(counts.retries.load() == 0);

    // Past the window the next tick retries; that attempt fails again and is
    // logged at debug rather than repeating the warning.
    std::this_thread::sleep_for(300ms);
    panel.refresh();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return counts.retries.load() >= 1;
                    }));
    CHECK(access.modules_calls->load() == kListingBeforeFirstFetch + 1);
    CHECK(counts.warnings.load() == 1);
    CHECK_FALSE(combo->isEnabled());

    // Past the doubled window the third attempt succeeds.
    access.modules_failing->store(false);
    std::this_thread::sleep_for(700ms);
    panel.refresh();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return combo->isEnabled();
                    }));
    CHECK(combo->count() == 3);
    CHECK(counts.warnings.load() == 1);
}

TEST_CASE("clearing the target stops the memory map retries", "[ui]")
{
    application();

    FakeAccess access;
    access.modules         = retry_modules();
    access.modules_failing = std::make_shared<std::atomic<bool>>(true);
    access.modules_calls   = std::make_shared<std::atomic<int>>(0);

    AccessWorker   worker {access};
    AttachedTarget target = fake_target();
    attach_app_session(worker);

    ScannerPanel panel {worker, target};

    auto* combo = range_combo(panel);
    REQUIRE(combo != nullptr);

    MapLogCounts     counts;
    const LevelGuard level_guard;
    const SinkGuard  sink_guard = map_log_sink(counts);
    slopkit::log::Logger::instance().set_minimum_level(slopkit::log::Level::info);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        panel.refresh();
                        return counts.warnings.load() >= 1;
                    }));
    const int warnings_before_clear = counts.warnings.load();

    target.clear();
    panel.refresh();

    CHECK(combo->count() == 1);
    CHECK(combo->itemText(0) == QStringLiteral("All memory"));
    CHECK_FALSE(combo->isEnabled());
    CHECK(panel.main_module_address() == 0U);

    // Past the backoff the cleared target must not be retried.
    std::this_thread::sleep_for(400ms);
    for (int tick = 0; tick < 5; ++tick)
    {
        panel.refresh();
        worker.drain();
        QCoreApplication::processEvents();
    }
    CHECK(access.modules_calls->load() == kListingBeforeFirstFetch);
    CHECK(counts.warnings.load() == warnings_before_clear);
    CHECK(combo->count() == 1);
    CHECK_FALSE(combo->isEnabled());
}
