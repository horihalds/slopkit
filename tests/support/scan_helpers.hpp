#pragma once

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
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "core/log.hpp"
#include "core/log_categories.hpp"
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

    [[maybe_unused]] void write_int32(std::vector<std::byte>& bytes, std::size_t offset, std::int32_t value)
    {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }

    [[maybe_unused]] std::int32_t read_int32(const std::vector<std::byte>& bytes, std::size_t offset)
    {
        std::int32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }

    [[maybe_unused]] ScanSnapshot wait(ScanEngine& engine)
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
    [[maybe_unused]] slopkit::scan::MemorySource mutable_source(std::shared_ptr<std::vector<std::byte>> bytes)
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

        [[maybe_unused]] void put_int32(std::uint64_t address, std::int32_t value)
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

    // A one-region source over an owned buffer with an unreadable hole in
    // [hole_begin, hole_end). A read that would touch the hole returns only the
    // readable prefix before it, exactly like a short process read.
    [[maybe_unused]] slopkit::scan::MemorySource hole_source(std::shared_ptr<std::vector<std::byte>> bytes,
                                                             std::uint64_t                           base,
                                                             std::uint64_t                           hole_begin,
                                                             std::uint64_t                           hole_end)
    {
        slopkit::scan::MemorySource source;
        const auto readable_count = [hole_begin, hole_end](std::uint64_t address, std::size_t size) -> std::size_t
        {
            const std::uint64_t end  = address + size;
            std::uint64_t       stop = end;
            if (address < hole_begin && end > hole_begin)
            {
                stop = hole_begin; // the read stops at the hole
            }
            else if (address >= hole_begin && address < hole_end)
            {
                stop = address; // starting inside the hole reads nothing
            }
            return static_cast<std::size_t>(stop - address);
        };
        source.read = [bytes, base, readable_count](
                          std::uint64_t address, std::size_t size) -> std::expected<std::vector<std::byte>, AccessError>
        {
            if (address < base || address - base + size > bytes->size())
            {
                return std::unexpected(AccessError::not_found);
            }
            const auto        offset = static_cast<std::size_t>(address - base);
            const std::size_t count  = readable_count(address, size);
            return std::vector<std::byte>(bytes->begin() + static_cast<std::ptrdiff_t>(offset),
                                          bytes->begin() + static_cast<std::ptrdiff_t>(offset + count));
        };
        source.read_into =
            [bytes, base, readable_count](std::uint64_t        address,
                                          std::span<std::byte> buffer) -> std::expected<std::size_t, AccessError>
        {
            if (address < base || address - base + buffer.size() > bytes->size())
            {
                return std::unexpected(AccessError::not_found);
            }
            const auto        offset = static_cast<std::size_t>(address - base);
            const std::size_t count  = readable_count(address, buffer.size());
            std::copy_n(bytes->begin() + static_cast<std::ptrdiff_t>(offset),
                        static_cast<std::ptrdiff_t>(count),
                        buffer.begin());
            return count;
        };
        source.regions = [bytes, base]()
        {
            RegionInfo region;
            region.start    = base;
            region.end      = base + bytes->size();
            region.readable = true;
            region.writable = true;
            return std::vector<RegionInfo> {region};
        };
        return source;
    }

    [[maybe_unused]] ScanConfig exact_config(ValueType type, std::int64_t value)
    {
        ScanConfig config;
        config.type       = ScanType::exact_value;
        config.value_type = type;
        config.value      = value;
        return config;
    }

    [[maybe_unused]] ScanConfig refine_config(ScanType type, ValueType value_type)
    {
        ScanConfig config;
        config.type       = type;
        config.value_type = value_type;
        return config;
    }

    // Read-call and byte counters a counting_source wrapper fills in.
    struct ReadCounter
    {
        std::atomic<std::size_t> read_calls {0};
        std::atomic<std::size_t> read_into_calls {0};
        std::atomic<std::size_t> bytes_read {0};
    };

    // Wraps `inner` and counts the read calls and bytes it hands out, so a test
    // can assert that a refinement coalesces its candidates into a few reads.
    [[maybe_unused]] slopkit::scan::MemorySource counting_source(slopkit::scan::MemorySource  inner,
                                                                 std::shared_ptr<ReadCounter> counter)
    {
        slopkit::scan::MemorySource source;
        if (inner.read)
        {
            auto read   = inner.read;
            source.read = [read, counter](std::uint64_t address,
                                          std::size_t   size) -> std::expected<std::vector<std::byte>, AccessError>
            {
                counter->read_calls.fetch_add(1, std::memory_order_relaxed);
                auto result = read(address, size);
                if (result)
                {
                    counter->bytes_read.fetch_add(result->size(), std::memory_order_relaxed);
                }
                return result;
            };
        }
        if (inner.read_into)
        {
            auto read_into   = inner.read_into;
            source.read_into = [read_into,
                                counter](std::uint64_t        address,
                                         std::span<std::byte> buffer) -> std::expected<std::size_t, AccessError>
            {
                counter->read_into_calls.fetch_add(1, std::memory_order_relaxed);
                auto result = read_into(address, buffer);
                if (result)
                {
                    counter->bytes_read.fetch_add(*result, std::memory_order_relaxed);
                }
                return result;
            };
        }
        source.regions = inner.regions;
        return source;
    }

    // Restores the process-wide log level on scope exit.
    class LevelGuard
    {
    public:
        LevelGuard() : previous_(slopkit::log::Logger::instance().minimum_level()) {}

        LevelGuard(const LevelGuard&)            = delete;
        LevelGuard& operator=(const LevelGuard&) = delete;

        [[maybe_unused]] ~LevelGuard()
        {
            slopkit::log::Logger::instance().set_minimum_level(previous_);
        }

    private:
        slopkit::log::Level previous_;
    };

    // Registers a sink for the lifetime of the guard.
    class SinkGuard
    {
    public:
        explicit SinkGuard(slopkit::log::Sink sink) : id_(slopkit::log::Logger::instance().add_sink(std::move(sink))) {}

        SinkGuard(const SinkGuard&)            = delete;
        SinkGuard& operator=(const SinkGuard&) = delete;

        [[maybe_unused]] ~SinkGuard()
        {
            slopkit::log::Logger::instance().remove_sink(id_);
        }

    private:
        slopkit::log::SinkId id_;
    };
} // namespace
