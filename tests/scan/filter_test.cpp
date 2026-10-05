#include <catch2/catch.hpp>

#include "support/scan_helpers.hpp"

TEST_CASE("region flags, address range and alignment restrict the scan", "[scan]")
{
    FakeSpace space;
    space.regions = {
        RegionInfo {0x1000, 0x2000, 0, true,  true, false, false,                 ""},
        RegionInfo {0x2000, 0x3000, 0, true, false,  true, false,                 ""},
        RegionInfo {0x3000, 0x4000, 0, true,  true, false, false, "/usr/lib/libx.so"},
        RegionInfo {0x4000, 0x5000, 0, true, false, false, false,                 ""},
    };
    space.put_int32(0x1100, 0x11223344);
    space.put_int32(0x2100, 0x11223344);
    space.put_int32(0x3100, 0x11223344);
    space.put_int32(0x4100, 0x11223344);
    space.put_int32(0x4202, 0x55667788);

    ScanConfig config = exact_config(ValueType::int32, 0x11223344);
    config.filter     = slopkit::scan::RegionFilter {};

    SECTION("writable only")
    {
        config.filter.writable      = true;
        config.filter.executable    = false;
        config.filter.copy_on_write = false;
        ScanEngine engine;
        engine.first_scan(config, space.source());
        const auto snapshot = wait(engine);
        CHECK(snapshot.hit_count == 2);
        CHECK(snapshot.total_bytes == 0x2000);
    }

    SECTION("executable only")
    {
        config.filter.writable      = false;
        config.filter.executable    = true;
        config.filter.copy_on_write = false;
        ScanEngine engine;
        engine.first_scan(config, space.source());
        const auto snapshot = wait(engine);
        REQUIRE(snapshot.hit_count == 1);
        CHECK(snapshot.hits[0].address == 0x2100);
    }

    SECTION("copy-on-write only")
    {
        config.filter.writable      = false;
        config.filter.executable    = false;
        config.filter.copy_on_write = true;
        ScanEngine engine;
        engine.first_scan(config, space.source());
        const auto snapshot = wait(engine);
        REQUIRE(snapshot.hit_count == 1);
        CHECK(snapshot.hits[0].address == 0x3100);
    }

    SECTION("no attribute filter scans every readable region")
    {
        config.filter.writable      = false;
        config.filter.executable    = false;
        config.filter.copy_on_write = false;
        ScanEngine engine;
        engine.first_scan(config, space.source());
        CHECK(wait(engine).hit_count == 4);
    }

    SECTION("the address range restricts the scan")
    {
        config.filter.writable      = false;
        config.filter.executable    = false;
        config.filter.copy_on_write = false;
        config.filter.start         = 0x1080;
        config.filter.stop          = 0x2200;
        ScanEngine engine;
        engine.first_scan(config, space.source());
        const auto snapshot = wait(engine);
        REQUIRE(snapshot.hit_count == 2);
        CHECK(snapshot.hits[0].address == 0x1100);
        CHECK(snapshot.hits[1].address == 0x2100);
    }

    SECTION("alignment skips unaligned matches")
    {
        config.filter.writable      = false;
        config.filter.executable    = false;
        config.filter.copy_on_write = false;
        config.value                = std::int64_t {0x55667788};
        config.filter.alignment     = 4;
        ScanEngine aligned;
        aligned.first_scan(config, space.source());
        CHECK(wait(aligned).hit_count == 0);

        config.filter.alignment = 1;
        ScanEngine unaligned;
        unaligned.first_scan(config, space.source());
        const auto snapshot = wait(unaligned);
        REQUIRE(snapshot.hit_count == 1);
        CHECK(snapshot.hits[0].address == 0x4202);
    }
}

TEST_CASE("a whole-address-space scan stops at the last mapped region", "[scan]")
{
    // A sparse space: the region list decides where the scan reads, so the
    // user-space ceiling passed in the filter never becomes a stray read.
    FakeSpace space;
    space.bytes.assign(0x6000, std::byte {});
    space.regions = {
        RegionInfo {0x1000, 0x2000, 0, true, true, false, false, ""},
        RegionInfo {0x5000, 0x6000, 0, true, true, false, false, ""},
    };
    space.put_int32(0x1100, 0x11223344);
    space.put_int32(0x5100, 0x11223344);

    ScanConfig config   = exact_config(ValueType::int32, 0x11223344);
    config.filter.start = 0x1000;
    config.filter.stop  = slopkit::scan::kMaxUserAddress;

    ScanEngine engine;
    engine.first_scan(config, space.source());

    const auto snapshot = wait(engine);
    REQUIRE(snapshot.state == ScanState::done);
    CHECK(snapshot.total_bytes == 0x2000);
    REQUIRE(snapshot.hit_count == 2);
    for (const auto& hit : snapshot.hits)
    {
        const bool mapped =
            (hit.address >= 0x1000 && hit.address < 0x2000) || (hit.address >= 0x5000 && hit.address < 0x6000);
        CHECK(mapped);
    }
}

TEST_CASE("cancelling a scan keeps the previous results", "[scan]")
{
    std::vector<std::byte> small(32);
    for (std::size_t i = 0; i < 8; ++i)
    {
        write_int32(small, i * 4, 10);
    }

    ScanEngine engine;
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

    engine.first_scan(exact_config(ValueType::int32, 0), slow);
    REQUIRE(engine.is_running());
    engine.cancel();

    const auto snapshot = wait(engine);
    CHECK(snapshot.state == ScanState::cancelled);
    CHECK(engine.result_count() == 8);
    CHECK(snapshot.hit_count == 8);
}
