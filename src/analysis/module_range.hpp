#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "process/types.hpp"

namespace slopkit::analysis
{
    // Mirrors `scan::ReadIntoFn` without pulling in `process/access.hpp`, so the
    // analysis stays a leaf usable off the worker thread.
    using ReadIntoFn =
        std::function<std::expected<std::size_t, process::AccessError>(std::uint64_t, std::span<std::byte>)>;

    struct ModuleRange
    {
        std::string   name;
        std::uint64_t base {};
        std::uint64_t end {};
    };

    enum class RegionReason
    {
        not_readable,
        not_executable,
        no_path,
        outside_module,
        empty_module,
    };

    struct ExcludedRegion
    {
        std::uint64_t start {};
        std::uint64_t end {};
        RegionReason  reason {RegionReason::outside_module};
    };

    // A target can map thousands of regions; the report keeps only enough
    // rejected regions to name the reasons without flooding the log.
    inline constexpr std::size_t kMaxExcludedRegions = 16;

    [[nodiscard]] constexpr std::string_view reason_name(RegionReason reason) noexcept
    {
        switch (reason)
        {
        case RegionReason::not_readable:
            return "not readable";
        case RegionReason::not_executable:
            return "not executable";
        case RegionReason::no_path:
            return "with no mapped file";
        case RegionReason::outside_module:
            return "outside the module";
        case RegionReason::empty_module:
            return "the module has no size";
        }
        return "unclassified";
    }

    [[nodiscard]] inline bool overlaps_module(const ModuleRange& module, const process::RegionInfo& region) noexcept
    {
        return region.start < module.end && region.end > module.base;
    }

    inline void
    exclude_region(std::vector<ExcludedRegion>& excluded, std::uint64_t start, std::uint64_t end, RegionReason reason)
    {
        if (excluded.size() < kMaxExcludedRegions)
        {
            excluded.push_back(ExcludedRegion {start, end, reason});
        }
    }

    [[nodiscard]] inline std::string describe_excluded_regions(std::span<const ExcludedRegion> excluded)
    {
        std::string skipped;
        for (const RegionReason reason : {RegionReason::empty_module,
                                          RegionReason::not_readable,
                                          RegionReason::not_executable,
                                          RegionReason::no_path,
                                          RegionReason::outside_module})
        {
            const std::size_t count = std::ranges::count(excluded, reason, &ExcludedRegion::reason);
            if (count == 0)
            {
                continue;
            }
            skipped += std::format("{}{} region(s) {}", skipped.empty() ? "" : ", ", count, reason_name(reason));
        }
        return skipped;
    }
}
