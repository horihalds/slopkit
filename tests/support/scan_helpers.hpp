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

    [[maybe_unused]] ScanConfig exact_config(ValueType type, std::int64_t value)
    {
        ScanConfig config;
        config.type       = ScanType::exact_value;
        config.value_type = type;
        config.value      = value;
        return config;
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
