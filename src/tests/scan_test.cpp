#include <catch2/catch.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "process/types.hpp"
#include "scan/engine.hpp"
#include "scan/matcher.hpp"
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

    // While the scan runs only the incremental page exists.
    const auto running = engine.snapshot();
    REQUIRE(running.state == ScanState::running);
    CHECK(running.result_hits == nullptr);

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

TEST_CASE("the compiled matcher agrees with the generic comparison", "[scan]")
{
    using slopkit::scan::Matcher;
    using slopkit::scan::ScanValue;

    std::mt19937                       rng(0xC0FFEEu);
    std::uniform_int_distribution<int> pick(0, 5);
    const std::array<int, 6>           alphabet {0x00, 0x01, 0x0F, 0x7F, 0xFF, 0x15};

    // The pre-matcher reference: the offsets the engine kept before, using the
    // fully generic comparison for every candidate.
    const auto reference =
        [](std::span<const std::byte> bytes, const ScanConfig& config, std::size_t window, std::size_t alignment)
    {
        std::vector<std::size_t> offsets;
        const bool               initial_unknown = config.type == ScanType::unknown_initial_value;
        for (std::size_t offset = 0; offset + window <= bytes.size(); offset += alignment)
        {
            if (initial_unknown
                || slopkit::scan::matches(config.type,
                                          config.value_type,
                                          bytes.subspan(offset, window),
                                          config.value,
                                          config.value_upper,
                                          config.hex))
            {
                offsets.push_back(offset);
            }
        }
        return offsets;
    };

    // Exactly what ScanEngine::run_first does with the matcher.
    const auto collected =
        [](std::span<const std::byte> bytes, const Matcher& matcher, std::size_t window, std::size_t alignment)
    {
        std::vector<std::size_t> offsets;
        if (matcher.matches_all())
        {
            for (std::size_t offset = 0; offset + window <= bytes.size(); offset += alignment)
            {
                offsets.push_back(offset);
            }
            return offsets;
        }
        for (std::size_t offset = 0; offset < bytes.size();)
        {
            const std::size_t hit = matcher.find(bytes, offset, bytes.size(), alignment);
            if (hit == bytes.size())
            {
                break;
            }
            offsets.push_back(hit);
            offset = hit + alignment;
        }
        return offsets;
    };

    struct Needle
    {
        ValueType type;
        ScanValue value;
    };

    const double              nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<Needle> needles {
        {      ValueType::byte,                                            ScanValue {std::int64_t {0}}},
        {      ValueType::byte,                                         ScanValue {std::int64_t {0x15}}},
        {      ValueType::byte,                                           ScanValue {std::int64_t {-1}}},
        {     ValueType::int16,                                            ScanValue {std::int64_t {0}}},
        {     ValueType::int16,                                       ScanValue {std::int64_t {0x1515}}},
        {     ValueType::int16,                                           ScanValue {std::int64_t {-1}}},
        {     ValueType::int32,                                            ScanValue {std::int64_t {0}}},
        {     ValueType::int32,                                           ScanValue {std::int64_t {15}}},
        {     ValueType::int32,                                           ScanValue {std::int64_t {-1}}},
        {     ValueType::int32,                                   ScanValue {std::int64_t {0x01020304}}},
        {     ValueType::int64,                                            ScanValue {std::int64_t {0}}},
        {     ValueType::int64,                                           ScanValue {std::int64_t {15}}},
        {     ValueType::int64,                                           ScanValue {std::int64_t {-1}}},
        {       ValueType::all,                                            ScanValue {std::int64_t {0}}},
        {       ValueType::all,                                           ScanValue {std::int64_t {15}}},
        {   ValueType::float32,                                                         ScanValue {0.0}},
        {   ValueType::float32,                                                        ScanValue {-0.0}},
        {   ValueType::float32,                                                         ScanValue {1.5}},
        {   ValueType::float32,                                                         ScanValue {nan}},
        {   ValueType::float64,                                                         ScanValue {0.0}},
        {   ValueType::float64,                                                        ScanValue {-0.0}},
        {   ValueType::float64,                                                         ScanValue {1.5}},
        {   ValueType::float64,                                                         ScanValue {nan}},
        {    ValueType::string,                                         ScanValue {std::string {"abc"}}},
        {    ValueType::string,                                           ScanValue {std::string {"a"}}},
        {ValueType::byte_array, ScanValue {std::vector<std::byte> {std::byte {0x15}, std::byte {0x00}}}},
        {ValueType::byte_array, ScanValue {std::vector<std::byte> {std::byte {0xFF}, std::byte {0xFF}}}},
        {ValueType::byte_array,                   ScanValue {std::vector<std::byte> {std::byte {0x00}}}},
    };

    const std::array<ScanType, 5>    scan_types {ScanType::exact_value,
                                                 ScanType::bigger_than,
                                                 ScanType::smaller_than,
                                                 ScanType::value_between,
                                                 ScanType::unknown_initial_value};
    const std::array<std::size_t, 4> alignments {1, 2, 4, 8};

    for (const auto& needle : needles)
    {
        for (const ScanType scan_type : scan_types)
        {
            ScanConfig config;
            config.type        = scan_type;
            config.value_type  = needle.type;
            config.value       = needle.value;
            config.value_upper = needle.value;
            if (std::holds_alternative<std::int64_t>(needle.value))
            {
                config.value_upper = ScanValue {std::get<std::int64_t>(needle.value) + 1};
            }
            else if (std::holds_alternative<double>(needle.value))
            {
                config.value_upper = ScanValue {std::get<double>(needle.value) + 1.0};
            }

            const std::size_t window = slopkit::scan::effective_size(config.value_type, config.value);
            if (window == 0)
            {
                continue;
            }

            const Matcher matcher = Matcher::build(config);
            REQUIRE(matcher.window() == window);
            REQUIRE(matcher.matches_all() == (scan_type == ScanType::unknown_initial_value));

            const std::vector<std::byte> needle_bytes = slopkit::scan::encode_value(config.value_type, config.value);

            for (const std::size_t alignment : alignments)
            {
                std::vector<std::byte> buffer(2048);
                for (auto& byte : buffer)
                {
                    byte = static_cast<std::byte>(alphabet[static_cast<std::size_t>(pick(rng))]);
                }
                if (!needle_bytes.empty() && needle_bytes.size() <= buffer.size())
                {
                    const std::size_t last = buffer.size() - needle_bytes.size();
                    for (const std::size_t at : {std::size_t {0}, std::size_t {64}, last})
                    {
                        std::copy(
                            needle_bytes.begin(), needle_bytes.end(), buffer.begin() + static_cast<std::ptrdiff_t>(at));
                    }
                }
                const std::span<const std::byte> bytes(buffer);

                INFO("value_type=" << static_cast<int>(needle.type) << " scan_type=" << static_cast<int>(scan_type)
                                   << " alignment=" << alignment);

                const bool initial_unknown = scan_type == ScanType::unknown_initial_value;
                bool       match_agrees    = true;
                for (std::size_t offset = 0; offset + window <= bytes.size(); ++offset)
                {
                    const bool expected = initial_unknown
                                       || slopkit::scan::matches(config.type,
                                                                 config.value_type,
                                                                 bytes.subspan(offset, window),
                                                                 config.value,
                                                                 config.value_upper,
                                                                 config.hex);
                    if (matcher.match(bytes.subspan(offset, window)) != expected)
                    {
                        match_agrees = false;
                        break;
                    }
                }
                CHECK(match_agrees);
                CHECK(collected(bytes, matcher, window, alignment) == reference(bytes, config, window, alignment));
            }
        }
    }
}

TEST_CASE("the matcher handles uniform and floating-point needles", "[scan]")
{
    using slopkit::scan::Matcher;
    using slopkit::scan::ScanValue;

    SECTION("a uniformly zero needle keeps every aligned hit")
    {
        const std::vector<std::byte>     bytes(64);
        const ScanConfig                 config  = exact_config(ValueType::int32, 0);
        const Matcher                    matcher = Matcher::build(config);
        const std::span<const std::byte> span(bytes);
        for (std::size_t offset = 0; offset + 4 <= bytes.size(); offset += 4)
        {
            CHECK(matcher.find(span, offset, span.size(), 4) == offset);
        }
        CHECK(matcher.find(span, 0, span.size(), 4) == 0);
    }

    SECTION("a uniformly 0xFF needle keeps every aligned hit")
    {
        const std::vector<std::byte>     bytes(64, std::byte {0xFF});
        const ScanConfig                 config  = exact_config(ValueType::int32, -1);
        const Matcher                    matcher = Matcher::build(config);
        const std::span<const std::byte> span(bytes);
        for (std::size_t offset = 0; offset + 4 <= bytes.size(); offset += 4)
        {
            CHECK(matcher.find(span, offset, span.size(), 4) == offset);
        }
    }

    SECTION("-0.0 equals 0.0 and NaN never matches")
    {
        std::vector<std::byte> bytes(16);
        bytes[0] = std::byte {0x00}; // 0.0 as a zero-filled double.

        const std::span<const std::byte> span(bytes);

        ScanConfig positive;
        positive.type       = ScanType::exact_value;
        positive.value_type = ValueType::float64;
        positive.value      = ScanValue {0.0};
        CHECK(Matcher::build(positive).match(span.subspan(0, 8)));

        ScanConfig negative;
        negative.type       = ScanType::exact_value;
        negative.value_type = ValueType::float64;
        negative.value      = ScanValue {-0.0};
        CHECK(Matcher::build(negative).match(span.subspan(0, 8)));

        ScanConfig not_a_number;
        not_a_number.type         = ScanType::exact_value;
        not_a_number.value_type   = ValueType::float64;
        not_a_number.value        = ScanValue {std::numeric_limits<double>::quiet_NaN()};
        const Matcher nan_matcher = Matcher::build(not_a_number);
        CHECK_FALSE(nan_matcher.match(span.subspan(0, 8)));
        CHECK_FALSE(nan_matcher.match(span.subspan(8, 8)));
    }
}

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
