#include "analysis/pointer_scan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "core/log_categories.hpp"

namespace slopkit::analysis
{
    namespace
    {
        struct IndexedPointer
        {
            std::uint64_t value {};
            std::uint64_t slot {};
        };

        struct BaseRange
        {
            std::uint64_t start {};
            std::uint64_t end {};
        };

        [[nodiscard]] constexpr std::uint64_t align_up(std::uint64_t address, std::size_t alignment) noexcept
        {
            const std::uint64_t mask = static_cast<std::uint64_t>(alignment) - 1;
            return (address + mask) & ~mask;
        }

        [[nodiscard]] std::uint64_t decode_pointer(std::span<const std::byte> bytes) noexcept
        {
            std::uint64_t value = 0;
            for (std::size_t index = bytes.size(); index > 0; --index)
            {
                value = (value << 8) | std::to_integer<std::uint64_t>(bytes[index - 1]);
            }
            return value;
        }

        [[nodiscard]] bool is_data_region(const ModuleRange& module, const process::RegionInfo& region) noexcept
        {
            return overlaps_module(module, region) && region.readable && region.writable && !region.executable;
        }

        [[nodiscard]] bool in_ranges(std::span<const BaseRange> ranges, std::uint64_t address) noexcept
        {
            return std::ranges::any_of(ranges,
                                       [address](const BaseRange& range)
                                       {
                                           return address >= range.start && address < range.end;
                                       });
        }
    }

    std::string describe(const PointerScanReport& report)
    {
        std::string text = std::format("{} writable region(s), {} data region(s), {} byte(s), {} pointer slot(s), "
                                       "{} match(es), {} path(s), {} node(s)",
                                       report.regions_scanned,
                                       report.data_regions,
                                       report.bytes_read,
                                       report.pointer_slots,
                                       report.matched_slots,
                                       report.paths,
                                       report.visited_nodes);
        if (report.read_failures != 0)
        {
            text += std::format(", {} unreadable chunk(s)", report.read_failures);
        }
        if (report.truncated)
        {
            text += ", truncated";
        }
        if (!report.excluded.empty())
        {
            text += std::format("; skipped {}", describe_excluded_regions(report.excluded));
        }
        return text;
    }

    PointerScanResult find_pointers(ReadIntoFn                           read,
                                    const ModuleRange&                   module,
                                    std::span<const process::RegionInfo> regions,
                                    std::uint64_t                        target,
                                    const PointerScanConfig&             config)
    {
        PointerScanResult  scan;
        PointerScanReport& report = scan.report;

        const std::size_t  pointer_size = config.pointer_size == 4 ? 4 : 8;
        const std::size_t  levels       = std::max<std::size_t>(1, config.levels);
        const std::int64_t max_offset   = std::max<std::int64_t>(0, config.max_offset);
        const std::size_t  chunk_size = std::max(pointer_size, config.chunk_size - (config.chunk_size % pointer_size));

        if (module.end <= module.base)
        {
            exclude_region(report.excluded, module.base, module.end, RegionReason::empty_module);
            log::debug(log::category::scan, std::format("pointer scan of {}: {}", module.name, describe(report)));
            return scan;
        }

        std::vector<BaseRange> bases;
        for (const process::RegionInfo& region : regions)
        {
            if (region.start >= region.end)
            {
                exclude_region(report.excluded, region.start, region.end, RegionReason::empty_module);
            }
            else if (is_data_region(module, region))
            {
                const std::uint64_t start = std::max(region.start, module.base);
                const std::uint64_t end   = std::min(region.end, module.end);
                if (start < end)
                {
                    bases.push_back(BaseRange {start, end});
                }
            }
        }
        if (bases.empty())
        {
            exclude_region(report.excluded, module.base, module.end, RegionReason::not_readable);
            log::debug(log::category::scan, std::format("pointer scan of {}: {}", module.name, describe(report)));
            return scan;
        }
        report.data_regions = bases.size();

        std::vector<IndexedPointer> index;
        std::vector<std::byte>      buffer(chunk_size);

        for (const process::RegionInfo& region : regions)
        {
            if (!region.readable || !region.writable || region.end <= region.start)
            {
                continue;
            }
            ++report.regions_scanned;

            std::uint64_t slot = align_up(region.start, pointer_size);
            while (slot + pointer_size <= region.end)
            {
                std::size_t want = static_cast<std::size_t>(region.end - slot);
                want             = std::min(want, chunk_size);
                want -= want % pointer_size;
                if (want == 0)
                {
                    break;
                }

                std::size_t got = 0;
                if (const auto chunk = read(slot, std::span<std::byte>(buffer.data(), want)); chunk.has_value())
                {
                    got = std::min<std::size_t>(*chunk, want);
                }
                if (got == 0)
                {
                    ++report.read_failures;
                    log::debug(log::category::scan,
                               std::format("pointer scan of {}: unreadable chunk at {:#x}", module.name, slot));
                    slot += want;
                    continue;
                }
                report.bytes_read += got;

                const std::size_t count = got / pointer_size;
                index.reserve(index.size() + count);
                for (std::size_t offset = 0; offset < count * pointer_size; offset += pointer_size)
                {
                    index.push_back(IndexedPointer {
                        .value = decode_pointer(std::span<const std::byte>(buffer.data() + offset, pointer_size)),
                        .slot  = slot + offset,
                    });
                }
                slot += count * pointer_size;
                if (got < want)
                {
                    break;
                }
            }
        }

        report.pointer_slots = index.size();
        std::ranges::sort(index,
                          [](const IndexedPointer& lhs, const IndexedPointer& rhs)
                          {
                              if (lhs.value != rhs.value)
                              {
                                  return lhs.value < rhs.value;
                              }
                              return lhs.slot < rhs.slot;
                          });

        const auto matching = [&](std::uint64_t current)
        {
            const std::uint64_t span = static_cast<std::uint64_t>(max_offset);
            const std::uint64_t high = span > std::numeric_limits<std::uint64_t>::max() - current
                                         ? std::numeric_limits<std::uint64_t>::max()
                                         : current + span;
            const std::uint64_t low  = config.allow_negative ? (span > current ? 0 : current - span) : current;
            return std::pair {std::ranges::lower_bound(index, low, {}, &IndexedPointer::value),
                              std::ranges::upper_bound(index, high, {}, &IndexedPointer::value)};
        };

        std::vector<std::int64_t> outward;
        bool                      stop = false;

        const auto walk = [&](auto&& self, std::uint64_t current, std::size_t depth) -> void
        {
            if (stop)
            {
                return;
            }
            if (report.visited_nodes >= config.max_nodes)
            {
                report.truncated = true;
                stop             = true;
                return;
            }
            ++report.visited_nodes;

            const auto [first, last] = matching(current);
            for (auto it = first; it != last; ++it)
            {
                const IndexedPointer& entry = *it;
                if (entry.value == 0)
                {
                    continue;
                }
                ++report.matched_slots;

                const std::int64_t offset = static_cast<std::int64_t>(current - entry.value);
                outward.push_back(offset);

                if (depth + 1 >= levels)
                {
                    if (in_ranges(bases, entry.slot))
                    {
                        PointerPath path;
                        path.base   = entry.slot;
                        path.target = target;
                        path.offsets.assign(outward.rbegin(), outward.rend());
                        scan.paths.push_back(std::move(path));
                        if (scan.paths.size() >= config.max_results)
                        {
                            report.truncated = true;
                            stop             = true;
                        }
                    }
                }
                else
                {
                    self(self, entry.slot, depth + 1);
                }

                outward.pop_back();
                if (stop)
                {
                    return;
                }
            }
        };

        walk(walk, target, 0);
        report.paths = scan.paths.size();
        log::debug(log::category::scan, std::format("pointer scan of {}: {}", module.name, describe(report)));
        return scan;
    }
}
