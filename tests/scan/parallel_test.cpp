#include <catch2/catch.hpp>

#include "support/scan_helpers.hpp"

TEST_CASE("a parallel first scan matches the sequential one", "[scan]")
{
    // Large enough to span two 32 MB shards so the pool really has work to
    // hand out, with one sparse `int32 == 15` needle per MiB.
    constexpr std::size_t  kBufferBytes = 40u << 20;
    std::vector<std::byte> bytes(kBufferBytes);
    std::size_t            expected = 0;
    for (std::size_t offset = 0; offset + sizeof(std::int32_t) <= bytes.size(); offset += (1u << 20))
    {
        write_int32(bytes, offset, 15);
        ++expected;
    }

    const auto       source = slopkit::scan::make_buffer_source(bytes, kBase);
    const ScanConfig config = exact_config(ValueType::int32, 15);

    std::vector<std::uint64_t> reference;
    {
        ScanEngine engine;
        engine.set_max_threads(1);
        engine.first_scan(config, source);
        const auto snapshot = wait(engine);
        REQUIRE(snapshot.state == ScanState::done);
        REQUIRE(snapshot.hit_count == expected);
        for (const auto& hit : snapshot.hits)
        {
            reference.push_back(hit.address);
        }
    }

    for (const std::size_t threads : {std::size_t {2}, std::size_t {4}, std::size_t {8}})
    {
        ScanEngine engine;
        engine.set_max_threads(threads);
        engine.first_scan(config, source);
        const auto snapshot = wait(engine);
        REQUIRE(snapshot.state == ScanState::done);
        CHECK(snapshot.hit_count == expected);

        std::vector<std::uint64_t> addresses;
        for (const auto& hit : snapshot.hits)
        {
            addresses.push_back(hit.address);
        }
        CHECK(addresses == reference);
    }
}

TEST_CASE("a match straddling a chunk or shard boundary is found", "[scan]")
{
    constexpr std::size_t  kChunkBytes = 4u << 20;
    constexpr std::size_t  kShardBytes = 1u << 25;
    constexpr std::int64_t kNeedle     = 0x0102030405060708LL;

    // An 8-byte window with alignment 4 straddles the boundary when it starts
    // four bytes before it; without the boundary overlap both would be missed.
    const std::array<std::size_t, 2> starts {kChunkBytes - 4, kShardBytes - 4};
    std::vector<std::byte>           bytes(kShardBytes + kChunkBytes + 64);
    for (const std::size_t at : starts)
    {
        std::memcpy(bytes.data() + at, &kNeedle, sizeof(kNeedle));
    }

    ScanConfig config;
    config.type             = ScanType::exact_value;
    config.value_type       = ValueType::int64;
    config.value            = kNeedle;
    config.filter.alignment = 4;

    const auto source = slopkit::scan::make_buffer_source(bytes, kBase);
    for (const std::size_t threads : {std::size_t {1}, std::size_t {4}})
    {
        ScanEngine engine;
        engine.set_max_threads(threads);
        engine.first_scan(config, source);
        const auto snapshot = wait(engine);
        REQUIRE(snapshot.state == ScanState::done);
        REQUIRE(snapshot.hit_count == 2);
        CHECK(snapshot.hits[0].address == kBase + starts[0]);
        CHECK(snapshot.hits[1].address == kBase + starts[1]);
    }
}

TEST_CASE("a parallel scan can be cancelled", "[scan]")
{
    std::vector<std::byte> small(32);
    for (std::size_t i = 0; i < 8; ++i)
    {
        write_int32(small, i * 4, 10);
    }

    ScanEngine engine;
    engine.set_max_threads(4);
    engine.first_scan(exact_config(ValueType::int32, 10), slopkit::scan::make_buffer_source(small, kBase));
    REQUIRE(wait(engine).hit_count == 8);

    // A large, deliberately slow source that the cancellation can interrupt.
    auto                        data = std::make_shared<std::vector<std::byte>>(8u << 20);
    slopkit::scan::MemorySource slow;
    slow.read = [data](std::uint64_t address, std::size_t size) -> std::expected<std::vector<std::byte>, AccessError>
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (address < kBase || address - kBase + size > data->size())
        {
            return std::unexpected(AccessError::not_found);
        }
        const auto offset = static_cast<std::size_t>(address - kBase);
        return std::vector<std::byte>(data->begin() + static_cast<std::ptrdiff_t>(offset),
                                      data->begin() + static_cast<std::ptrdiff_t>(offset + size));
    };
    slow.regions = [data]()
    {
        RegionInfo region;
        region.start    = kBase;
        region.end      = kBase + data->size();
        region.readable = true;
        region.writable = true;
        return std::vector<RegionInfo> {region};
    };

    engine.first_scan(exact_config(ValueType::int32, 15), slow);
    REQUIRE(engine.is_running());
    engine.cancel();

    const auto snapshot = wait(engine);
    CHECK(snapshot.state == ScanState::cancelled);
    CHECK(engine.result_count() == 8);
    CHECK(snapshot.hit_count == 8);
}

TEST_CASE("scanned bytes advance monotonically during a scan", "[scan]")
{
    auto                        data = std::make_shared<std::vector<std::byte>>(4u << 20);
    slopkit::scan::MemorySource slow;
    slow.read = [data](std::uint64_t address, std::size_t size) -> std::expected<std::vector<std::byte>, AccessError>
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (address < kBase || address - kBase + size > data->size())
        {
            return std::unexpected(AccessError::not_found);
        }
        const auto offset = static_cast<std::size_t>(address - kBase);
        return std::vector<std::byte>(data->begin() + static_cast<std::ptrdiff_t>(offset),
                                      data->begin() + static_cast<std::ptrdiff_t>(offset + size));
    };
    slow.regions = [data]()
    {
        RegionInfo region;
        region.start    = kBase;
        region.end      = kBase + data->size();
        region.readable = true;
        region.writable = true;
        return std::vector<RegionInfo> {region};
    };

    ScanEngine engine;
    engine.set_max_threads(2);
    engine.first_scan(exact_config(ValueType::int32, 15), slow);

    std::size_t last      = 0;
    bool        monotonic = true;
    while (engine.is_running())
    {
        const auto snapshot = engine.snapshot();
        if (snapshot.scanned_bytes < last)
        {
            monotonic = false;
        }
        last = snapshot.scanned_bytes;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto snapshot = engine.snapshot();
    CHECK(monotonic);
    CHECK(snapshot.scanned_bytes == snapshot.total_bytes);
    CHECK(snapshot.progress == Approx(1.0f));
}
