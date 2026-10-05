#include <catch2/catch.hpp>

#include "support/scan_helpers.hpp"

TEST_CASE("a first scan finds exact values", "[scan]")
{
    std::vector<std::byte> bytes(32);
    for (std::size_t i = 0; i < 8; ++i)
    {
        write_int32(bytes, i * 4, static_cast<std::int32_t>(i * 10));
    }

    ScanEngine engine;
    engine.first_scan(exact_config(ValueType::int32, 20), slopkit::scan::make_buffer_source(bytes, kBase));

    const auto snapshot = wait(engine);
    REQUIRE(snapshot.state == ScanState::done);
    REQUIRE(snapshot.hit_count == 1);
    REQUIRE(snapshot.hits.size() == 1);
    CHECK(snapshot.hits[0].address == kBase + 8);
    CHECK(snapshot.hits[0].previous.empty());
    CHECK(snapshot.progress == Approx(1.0f));
}

TEST_CASE("a finished scan publishes the whole stored result set", "[scan]")
{
    // 300 matches, more than the display page.
    auto bytes = std::make_shared<std::vector<std::byte>>(300 * 4);
    for (std::size_t i = 0; i < 300; ++i)
    {
        write_int32(*bytes, i * 4, 10);
    }

    // Delay the first read so the running state is observable.
    auto                        delayed = std::make_shared<std::atomic<bool>>(false);
    slopkit::scan::MemorySource source;
    source.read = [bytes, delayed](std::uint64_t address,
                                   std::size_t   size) -> std::expected<std::vector<std::byte>, AccessError>
    {
        if (!delayed->exchange(true))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (address < kBase || address - kBase + size > bytes->size())
        {
            return std::unexpected(AccessError::not_found);
        }
        const auto offset = static_cast<std::size_t>(address - kBase);
        return std::vector<std::byte>(bytes->begin() + static_cast<std::ptrdiff_t>(offset),
                                      bytes->begin() + static_cast<std::ptrdiff_t>(offset + size));
    };
    source.regions = [bytes]()
    {
        RegionInfo region;
        region.start    = kBase;
        region.end      = kBase + bytes->size();
        region.readable = true;
        region.writable = true;
        return std::vector<RegionInfo> {region};
    };

    ScanEngine engine;
    ScanConfig config       = exact_config(ValueType::int32, 10);
    config.filter.alignment = 4;
    engine.first_scan(config, source);

    // While the scan runs no rows are published at all.
    const auto running = engine.snapshot();
    REQUIRE(running.state == ScanState::running);
    CHECK(running.hits.empty());
    CHECK(running.result_hits == nullptr);
    CHECK(running.progress >= 0.0f);
    CHECK(running.progress <= 1.0f);

    const auto snapshot = wait(engine);
    REQUIRE(snapshot.state == ScanState::done);
    REQUIRE(snapshot.hit_count == 300);
    CHECK(snapshot.hits.size() == slopkit::scan::kDisplayPage);
    REQUIRE(snapshot.result_hits != nullptr);
    CHECK(snapshot.result_hits->size() == 300);

    // The handle aliases the engine's storage, so taking another snapshot does
    // not copy the whole result set.
    CHECK(engine.snapshot().result_hits.get() == snapshot.result_hits.get());
}

TEST_CASE("a refinement publishes no rows before it finishes", "[scan]")
{
    // 300 matches, more than the display page.
    auto bytes = std::make_shared<std::vector<std::byte>>(300 * 4);
    for (std::size_t i = 0; i < 300; ++i)
    {
        write_int32(*bytes, i * 4, 10);
    }

    // Block the refinement's first read so its running state stays observable.
    auto                        block   = std::make_shared<std::atomic<bool>>(false);
    auto                        release = std::make_shared<std::atomic<bool>>(false);
    slopkit::scan::MemorySource source;
    source.read = [bytes, block, release](std::uint64_t address,
                                          std::size_t   size) -> std::expected<std::vector<std::byte>, AccessError>
    {
        if (block->load())
        {
            while (!release->load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (address < kBase || address - kBase + size > bytes->size())
        {
            return std::unexpected(AccessError::not_found);
        }
        const auto offset = static_cast<std::size_t>(address - kBase);
        return std::vector<std::byte>(bytes->begin() + static_cast<std::ptrdiff_t>(offset),
                                      bytes->begin() + static_cast<std::ptrdiff_t>(offset + size));
    };
    source.regions = [bytes]()
    {
        RegionInfo region;
        region.start    = kBase;
        region.end      = kBase + bytes->size();
        region.readable = true;
        region.writable = true;
        return std::vector<RegionInfo> {region};
    };

    ScanEngine engine;
    ScanConfig config       = exact_config(ValueType::int32, 10);
    config.filter.alignment = 4;
    engine.first_scan(config, source);
    const auto first = wait(engine);
    REQUIRE(first.state == ScanState::done);
    REQUIRE(first.result_hits != nullptr);
    REQUIRE(first.result_hits->size() == 300);

    block->store(true);
    ScanConfig refinement;
    refinement.type       = ScanType::unchanged;
    refinement.value_type = ValueType::int32;
    engine.next_scan(refinement);

    // The previous set is not exposed as a running page.
    const auto running = engine.snapshot();
    REQUIRE(running.state == ScanState::running);
    CHECK(running.hits.empty());
    CHECK(running.result_hits == nullptr);

    release->store(true);
    const auto refined = wait(engine);
    REQUIRE(refined.state == ScanState::done);
    REQUIRE(refined.result_hits != nullptr);
    CHECK(refined.result_hits->size() == 300);
}

TEST_CASE("undo republishes the whole result set through the handle", "[scan]")
{
    auto bytes = std::make_shared<std::vector<std::byte>>(300 * 4);
    for (std::size_t i = 0; i < 300; ++i)
    {
        write_int32(*bytes, i * 4, 10);
    }

    ScanEngine engine;
    ScanConfig config       = exact_config(ValueType::int32, 10);
    config.filter.alignment = 4;
    engine.first_scan(config, mutable_source(bytes));
    const auto first = wait(engine);
    REQUIRE(first.state == ScanState::done);
    REQUIRE(first.hit_count == 300);
    REQUIRE(first.result_hits != nullptr);
    CHECK(first.result_hits->size() == 300);

    // Rewrite half the values and keep the unchanged ones.
    for (std::size_t i = 0; i < 150; ++i)
    {
        write_int32(*bytes, i * 4, 20);
    }
    ScanConfig refinement;
    refinement.type       = ScanType::unchanged;
    refinement.value_type = ValueType::int32;
    engine.next_scan(refinement);
    const auto refined = wait(engine);
    REQUIRE(refined.state == ScanState::done);
    REQUIRE(refined.hit_count == 150);
    REQUIRE(refined.result_hits != nullptr);
    CHECK(refined.result_hits->size() == 150);

    engine.undo();
    const auto undone = engine.snapshot();
    REQUIRE(undone.state == ScanState::done);
    REQUIRE(undone.result_hits != nullptr);
    CHECK(undone.result_hits->size() == 300);
    CHECK(undone.hit_count == 300);
}

TEST_CASE("undo availability tracks the refinement history", "[scan]")
{
    auto bytes = std::make_shared<std::vector<std::byte>>(300 * 4);
    for (std::size_t i = 0; i < 300; ++i)
    {
        write_int32(*bytes, i * 4, 10);
    }

    ScanEngine engine;
    ScanConfig config       = exact_config(ValueType::int32, 10);
    config.filter.alignment = 4;

    // A first scan clears the history, so there is nothing to undo yet.
    engine.first_scan(config, mutable_source(bytes));
    REQUIRE(wait(engine).state == ScanState::done);
    CHECK_FALSE(engine.can_undo());

    // Refining pushes the previous set onto the history.
    ScanConfig refinement;
    refinement.type       = ScanType::unchanged;
    refinement.value_type = ValueType::int32;
    engine.next_scan(refinement);
    REQUIRE(wait(engine).state == ScanState::done);
    CHECK(engine.can_undo());

    // Undoing the only refinement exhausts the history again.
    engine.undo();
    CHECK_FALSE(engine.can_undo());
    REQUIRE(engine.has_results());

    // Another refinement makes undo available again; reset drops the history.
    engine.next_scan(refinement);
    REQUIRE(wait(engine).state == ScanState::done);
    CHECK(engine.can_undo());
    engine.reset();
    CHECK_FALSE(engine.can_undo());
}

TEST_CASE("the comparison scan types respect their bounds", "[scan]")
{
    std::vector<std::byte> bytes(32);
    for (std::size_t i = 0; i < 8; ++i)
    {
        write_int32(bytes, i * 4, static_cast<std::int32_t>(i * 10));
    }
    const auto source = slopkit::scan::make_buffer_source(bytes, kBase);

    SECTION("bigger than")
    {
        ScanConfig config       = exact_config(ValueType::int32, 40);
        config.type             = ScanType::bigger_than;
        config.filter.alignment = 4;
        ScanEngine engine;
        engine.first_scan(config, source);
        CHECK(wait(engine).hit_count == 3);
    }
    SECTION("smaller than")
    {
        ScanConfig config       = exact_config(ValueType::int32, 10);
        config.type             = ScanType::smaller_than;
        config.filter.alignment = 4;
        ScanEngine engine;
        engine.first_scan(config, source);
        CHECK(wait(engine).hit_count == 1);
    }
    SECTION("value between")
    {
        ScanConfig config       = exact_config(ValueType::int32, 20);
        config.type             = ScanType::value_between;
        config.value_upper      = std::int64_t {40};
        config.filter.alignment = 4;
        ScanEngine engine;
        engine.first_scan(config, source);
        CHECK(wait(engine).hit_count == 3);
    }
}

TEST_CASE("refinement scans narrow the result set and undo restores it", "[scan]")
{
    auto bytes = std::make_shared<std::vector<std::byte>>(32);
    for (std::size_t i = 0; i < 8; ++i)
    {
        write_int32(*bytes, i * 4, 10);
    }

    ScanEngine engine;
    ScanConfig unknown;
    unknown.type             = ScanType::unknown_initial_value;
    unknown.value_type       = ValueType::int32;
    unknown.filter.alignment = 4;
    engine.first_scan(unknown, mutable_source(bytes));
    CHECK(wait(engine).hit_count == 8);

    // Change a few values, leaving the rest at 10.
    write_int32(*bytes, 0, 20);
    write_int32(*bytes, 4, 30);
    write_int32(*bytes, 8, 0);
    write_int32(*bytes, 12, -5);

    SECTION("increased")
    {
        ScanConfig config;
        config.type       = ScanType::increased;
        config.value_type = ValueType::int32;
        engine.next_scan(config);
        const auto snapshot = wait(engine);
        REQUIRE(snapshot.hit_count == 2);
        CHECK(snapshot.hits[0].address == kBase);
        CHECK(read_int32(snapshot.hits[0].previous, 0) == 10);
    }

    SECTION("decreased")
    {
        ScanConfig config;
        config.type       = ScanType::decreased;
        config.value_type = ValueType::int32;
        engine.next_scan(config);
        REQUIRE(wait(engine).hit_count == 2);
    }

    SECTION("changed finds the four rewritten values and unchanged the rest")
    {
        ScanConfig changed;
        changed.type       = ScanType::changed;
        changed.value_type = ValueType::int32;
        engine.next_scan(changed);
        REQUIRE(wait(engine).hit_count == 4);

        engine.undo();
        CHECK(engine.snapshot().hit_count == 8);

        ScanConfig unchanged;
        unchanged.type       = ScanType::unchanged;
        unchanged.value_type = ValueType::int32;
        engine.next_scan(unchanged);
        CHECK(wait(engine).hit_count == 4);
    }
}

TEST_CASE("undo with no history keeps the result set", "[scan]")
{
    std::vector<std::byte> bytes(8);
    write_int32(bytes, 0, 7);

    ScanEngine engine;
    engine.first_scan(exact_config(ValueType::int32, 7), slopkit::scan::make_buffer_source(bytes, kBase));
    REQUIRE(wait(engine).hit_count == 1);

    engine.undo();
    CHECK(engine.snapshot().hit_count == 1);
}

TEST_CASE("next scan without a previous scan fails cleanly", "[scan]")
{
    SECTION("a next scan without a previous scan fails")
    {
        ScanEngine engine;
        ScanConfig config;
        config.type       = ScanType::increased;
        config.value_type = ValueType::int32;
        engine.next_scan(config);
        CHECK(engine.snapshot().state == ScanState::failed);
    }

    SECTION("a refinement as the first scan fails")
    {
        std::vector<std::byte> bytes(8);
        ScanEngine             engine;
        ScanConfig             config;
        config.type       = ScanType::increased;
        config.value_type = ValueType::int32;
        engine.first_scan(config, slopkit::scan::make_buffer_source(bytes, kBase));
        CHECK(wait(engine).state == ScanState::failed);
    }

    SECTION("a scan without a source fails")
    {
        ScanEngine engine;
        engine.first_scan(exact_config(ValueType::int32, 1), slopkit::scan::MemorySource {});
        CHECK(wait(engine).state == ScanState::failed);
    }
}

TEST_CASE("resetting the engine returns it to the constructed state", "[scan]")
{
    std::vector<std::byte> bytes(64);
    for (std::size_t i = 0; i < 8; ++i)
    {
        write_int32(bytes, i * 4, 10);
    }
    const auto source = slopkit::scan::make_buffer_source(bytes, kBase);

    ScanEngine engine;
    engine.first_scan(exact_config(ValueType::int32, 10), source);
    REQUIRE(wait(engine).hit_count == 8);
    REQUIRE(engine.has_results());

    // Refine once so the history is non-empty before the reset.
    engine.next_scan(exact_config(ValueType::int32, 10));
    REQUIRE(wait(engine).state == ScanState::done);
    REQUIRE(engine.has_results());

    engine.reset();

    CHECK_FALSE(engine.has_results());
    CHECK(engine.result_count() == 0);
    CHECK_FALSE(engine.is_running());
    const auto snapshot = engine.snapshot();
    CHECK(snapshot.state == ScanState::idle);
    CHECK(snapshot.hits.empty());
    CHECK(snapshot.hit_count == 0);
    CHECK(snapshot.progress == Approx(0.0f));

    // Undo is a no-op once the history is gone.
    engine.undo();
    CHECK_FALSE(engine.has_results());

    // A following first scan still works.
    engine.first_scan(exact_config(ValueType::int32, 10), source);
    CHECK(wait(engine).hit_count == 8);
}

TEST_CASE("scan throughput", "[.perf]")
{
    // A large synthetic address space with sparse `int32 == 15` needles, i.e.
    // the reported first-scan case (4-byte exact value, Fast Scan alignment 4).
    constexpr std::size_t   kBufferBytes = 512u << 20;
    constexpr std::size_t   kNeedleEvery = 1u << 20;
    constexpr std::int32_t  kNeedle      = 15;
    constexpr std::uint64_t kAlignment   = 4;

    std::vector<std::byte> buffer(kBufferBytes);
    std::size_t            expected = 0;
    for (std::size_t offset = 0; offset + sizeof(std::int32_t) <= buffer.size(); offset += kNeedleEvery)
    {
        write_int32(buffer, offset, kNeedle);
        ++expected;
    }

    ScanConfig config       = exact_config(ValueType::int32, kNeedle);
    config.filter.alignment = kAlignment;

    const auto source = slopkit::scan::make_buffer_source(buffer, kBase);

    std::cout << "scan throughput: " << (kBufferBytes >> 20) << " MiB buffer, " << expected << " needles\n";
    for (const std::size_t threads : {std::size_t {1}, std::size_t {2}, std::size_t {4}, std::size_t {8}})
    {
        ScanEngine engine;
        engine.set_max_threads(threads);

        const auto start = std::chrono::steady_clock::now();
        engine.first_scan(config, source);
        const auto snapshot = wait(engine);
        const auto elapsed  = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        REQUIRE(snapshot.state == ScanState::done);
        REQUIRE(snapshot.hit_count == expected);

        const double mib_per_second  = static_cast<double>(kBufferBytes) / (1024.0 * 1024.0) / elapsed;
        const double hits_per_second = static_cast<double>(snapshot.hit_count) / elapsed;
        std::cout << "  threads=" << threads << " time=" << elapsed << "s"
                  << " throughput=" << mib_per_second << " MiB/s"
                  << " hits/s=" << hits_per_second << "\n";
    }
}
