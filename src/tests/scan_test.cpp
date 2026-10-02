#include <catch2/catch.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "process/types.hpp"
#include "scan/engine.hpp"
#include "scan/source.hpp"
#include "scan/types.hpp"
#include "scan/value.hpp"

namespace
{
    using slopkit::process::AccessError;
    using slopkit::process::RegionInfo;
    using slopkit::scan::ScanConfig;
    using slopkit::scan::ScanEngine;
    using slopkit::scan::ScanSnapshot;
    using slopkit::scan::ScanState;
    using slopkit::scan::ScanType;
    using slopkit::scan::ValueType;

    constexpr std::uint64_t kBase = 0x1000;

    void write_int32(std::vector<std::byte>& bytes, std::size_t offset, std::int32_t value)
    {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }

    std::int32_t read_int32(const std::vector<std::byte>& bytes, std::size_t offset)
    {
        std::int32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }

    ScanSnapshot wait(ScanEngine& engine)
    {
        for (int i = 0; i < 5000; ++i)
        {
            const auto snapshot = engine.snapshot();
            if (snapshot.state != ScanState::running)
            {
                return snapshot;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return engine.snapshot();
    }

    // A source over an owned, mutable buffer so tests can change memory between
    // scans.
    slopkit::scan::MemorySource mutable_source(std::shared_ptr<std::vector<std::byte>> bytes)
    {
        slopkit::scan::MemorySource source;
        source.read = [bytes](std::uint64_t address,
                              std::size_t   size) -> std::expected<std::vector<std::byte>, AccessError>
        {
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
        return source;
    }

    // A synthetic address space with several regions of differing attributes.
    struct FakeSpace
    {
        std::uint64_t           base  = 0x1000;
        std::vector<std::byte>  bytes = std::vector<std::byte>(0x4000);
        std::vector<RegionInfo> regions;

        void put_int32(std::uint64_t address, std::int32_t value)
        {
            write_int32(bytes, static_cast<std::size_t>(address - base), value);
        }

        [[nodiscard]] slopkit::scan::MemorySource source() const
        {
            auto                        data = bytes;
            auto                        list = regions;
            slopkit::scan::MemorySource source;
            source.read = [data, base = base](std::uint64_t address,
                                              std::size_t   size) -> std::expected<std::vector<std::byte>, AccessError>
            {
                if (address < base || address - base + size > data.size())
                {
                    return std::unexpected(AccessError::not_found);
                }
                const auto offset = static_cast<std::size_t>(address - base);
                return std::vector<std::byte>(data.begin() + static_cast<std::ptrdiff_t>(offset),
                                              data.begin() + static_cast<std::ptrdiff_t>(offset + size));
            };
            source.regions = [list]()
            {
                return list;
            };
            return source;
        }
    };

    ScanConfig exact_config(ValueType type, std::int64_t value)
    {
        ScanConfig config;
        config.type       = ScanType::exact_value;
        config.value_type = type;
        config.value      = value;
        return config;
    }
} // namespace

TEST_CASE("value literals parse for every value type", "[scan]")
{
    CHECK(std::get<std::int64_t>(*slopkit::scan::parse_value(ValueType::byte, "42", false)) == 42);
    CHECK(std::get<std::int64_t>(*slopkit::scan::parse_value(ValueType::byte, "-1", false)) == -1);
    CHECK(std::get<std::int64_t>(*slopkit::scan::parse_value(ValueType::byte, "0x80", false)) == -128);
    CHECK(std::get<std::int64_t>(*slopkit::scan::parse_value(ValueType::int16, "0x1234", false)) == 0x1234);
    CHECK(std::get<std::int64_t>(*slopkit::scan::parse_value(ValueType::int32, "0xFFFFFFFF", false)) == -1);
    CHECK(std::get<std::int64_t>(*slopkit::scan::parse_value(ValueType::int64, "0xFFFFFFFFFFFFFFFF", false)) == -1);
    CHECK(std::get<double>(*slopkit::scan::parse_value(ValueType::float64, "3.5", false)) == 3.5);
    CHECK(std::get<double>(*slopkit::scan::parse_value(ValueType::float32, "-2.25", false)) == -2.25);
    CHECK(std::get<std::string>(*slopkit::scan::parse_value(ValueType::string, "hello", false)) == "hello");

    const auto bytes =
        std::get<std::vector<std::byte>>(*slopkit::scan::parse_value(ValueType::byte_array, "DE AD 01", true));
    REQUIRE(bytes.size() == 3);
    CHECK(static_cast<unsigned>(bytes[0]) == 0xDE);
    CHECK(static_cast<unsigned>(bytes[1]) == 0xAD);
    CHECK(static_cast<unsigned>(bytes[2]) == 0x01);
}

TEST_CASE("malformed values are reported instead of throwing", "[scan]")
{
    CHECK_FALSE(slopkit::scan::parse_value(ValueType::int32, "abc", false).has_value());
    CHECK_FALSE(slopkit::scan::parse_value(ValueType::int32, "", false).has_value());
    CHECK_FALSE(slopkit::scan::parse_value(ValueType::byte, "128", false).has_value());
    CHECK_FALSE(slopkit::scan::parse_value(ValueType::int32, "1e3", false).has_value());
    CHECK_FALSE(slopkit::scan::parse_value(ValueType::float64, "0x10", true).has_value());
    CHECK_FALSE(slopkit::scan::parse_value(ValueType::byte_array, "300", false).has_value());
}

TEST_CASE("values format back to text", "[scan]")
{
    std::vector<std::byte> bytes(4);
    write_int32(bytes, 0, 0x01020304);

    CHECK(slopkit::scan::format_value(ValueType::int32, bytes, false) == "16909060");
    CHECK(slopkit::scan::format_value(ValueType::int32, bytes, true) == "0x01020304");

    std::vector<std::byte> negative(4);
    write_int32(negative, 0, -1);
    CHECK(slopkit::scan::format_value(ValueType::int32, negative, false) == "-1");
    CHECK(slopkit::scan::format_value(ValueType::int32, negative, true) == "0xFFFFFFFF");
}

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
    ScanEngine engine;
    ScanConfig config;
    config.type       = ScanType::increased;
    config.value_type = ValueType::int32;
    engine.next_scan(config);
    CHECK(engine.snapshot().state == ScanState::failed);
}

TEST_CASE("a refinement as the first scan fails cleanly", "[scan]")
{
    std::vector<std::byte> bytes(8);
    ScanEngine             engine;
    ScanConfig             config;
    config.type       = ScanType::increased;
    config.value_type = ValueType::int32;
    engine.first_scan(config, slopkit::scan::make_buffer_source(bytes, kBase));
    CHECK(wait(engine).state == ScanState::failed);
}

TEST_CASE("a scan without a source fails cleanly", "[scan]")
{
    ScanEngine engine;
    engine.first_scan(exact_config(ValueType::int32, 1), slopkit::scan::MemorySource {});
    CHECK(wait(engine).state == ScanState::failed);
}

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
