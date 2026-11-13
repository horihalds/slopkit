#include "analysis/pointer_scan.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <limits>
#include <span>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "core/log_categories.hpp"

namespace slopkit::analysis
{
    namespace
    {
        struct Range
        {
            std::uint64_t start {};
            std::uint64_t end {};
        };

        struct IndexedPointer
        {
            std::uint64_t value {};
            std::uint64_t slot {};
        };

        struct Chunk
        {
            std::uint64_t start {};
            std::uint64_t end {};
        };

        struct Shard
        {
            std::vector<IndexedPointer> entries;
            std::size_t                 bytes_read {};
            std::size_t                 slots {};
            std::size_t                 read_failures {};
        };

        struct Edge
        {
            std::size_t  parent {};
            std::int64_t offset {};
        };

        struct Node
        {
            std::uint64_t     address {};
            std::vector<Edge> parents;
        };

        constexpr std::size_t kMaxThreads       = 8;
        constexpr std::size_t kReserveHintLimit = 4'000'000;

        [[nodiscard]] constexpr std::uint64_t align_up(std::uint64_t address, std::size_t alignment) noexcept
        {
            const std::uint64_t mask = static_cast<std::uint64_t>(alignment) - 1;
            return (address + mask) & ~mask;
        }

        [[nodiscard]] std::uint64_t decode_pointer(std::span<const std::byte> bytes) noexcept
        {
            if (bytes.size() == sizeof(std::uint64_t))
            {
                std::uint64_t value = 0;
                std::memcpy(&value, bytes.data(), sizeof(value));
                return value;
            }
            std::uint32_t value = 0;
            std::memcpy(&value, bytes.data(), sizeof(value));
            return value;
        }

        [[nodiscard]] bool is_data_region(const ModuleRange& module, const process::RegionInfo& region) noexcept
        {
            return overlaps_module(module, region) && region.readable && region.writable && !region.executable;
        }

        void add_range(std::vector<Range>& ranges, std::uint64_t start, std::uint64_t end)
        {
            if (start < end)
            {
                ranges.push_back(Range {start, end});
            }
        }

        void merge_ranges(std::vector<Range>& ranges)
        {
            if (ranges.empty())
            {
                return;
            }
            std::ranges::sort(ranges,
                              [](const Range& lhs, const Range& rhs)
                              {
                                  if (lhs.start != rhs.start)
                                  {
                                      return lhs.start < rhs.start;
                                  }
                                  return lhs.end < rhs.end;
                              });

            std::vector<Range> merged;
            merged.reserve(ranges.size());
            for (const Range& range : ranges)
            {
                if (merged.empty() || range.start > merged.back().end)
                {
                    merged.push_back(range);
                }
                else
                {
                    merged.back().end = std::max(merged.back().end, range.end);
                }
            }
            ranges = std::move(merged);
        }

        [[nodiscard]] bool in_ranges(std::span<const Range> ranges, std::uint64_t value) noexcept
        {
            const auto after = std::upper_bound(ranges.begin(),
                                                ranges.end(),
                                                value,
                                                [](std::uint64_t address, const Range& range)
                                                {
                                                    return address < range.start;
                                                });
            if (after == ranges.begin())
            {
                return false;
            }
            return value < std::prev(after)->end;
        }

        [[nodiscard]] std::uint64_t subtract_saturated(std::uint64_t value, std::uint64_t amount) noexcept
        {
            return amount > value ? 0 : value - amount;
        }

        [[nodiscard]] std::uint64_t add_saturated(std::uint64_t value, std::uint64_t amount) noexcept
        {
            return amount > std::numeric_limits<std::uint64_t>::max() - value
                     ? std::numeric_limits<std::uint64_t>::max()
                     : value + amount;
        }

        void add_expanded_range(std::vector<Range>& ranges, const Range& range, std::uint64_t span)
        {
            add_range(ranges, subtract_saturated(range.start, span), add_saturated(range.end, span));
        }

        void radix_sort(std::vector<IndexedPointer>& entries)
        {
            if (entries.size() < 2)
            {
                return;
            }

            std::vector<IndexedPointer> temporary(entries.size());
            for (unsigned shift = 0; shift < 64; shift += 8)
            {
                std::array<std::size_t, 256> counts {};
                for (const IndexedPointer& entry : entries)
                {
                    ++counts[(entry.value >> shift) & 0xFF];
                }

                std::size_t offset = 0;
                for (std::size_t& count : counts)
                {
                    const std::size_t size = count;
                    count                  = offset;
                    offset += size;
                }

                for (const IndexedPointer& entry : entries)
                {
                    temporary[counts[(entry.value >> shift) & 0xFF]++] = entry;
                }
                entries.swap(temporary);
            }
        }

        [[nodiscard]] bool path_less(const PointerPath& lhs, const PointerPath& rhs)
        {
            if (lhs.base != rhs.base)
            {
                return lhs.base < rhs.base;
            }
            return std::lexicographical_compare(
                lhs.offsets.begin(), lhs.offsets.end(), rhs.offsets.begin(), rhs.offsets.end());
        }
    }

    std::string describe(const PointerScanReport& report)
    {
        std::string text = std::format("{} writable region(s), {} data region(s), {} byte(s), {} pointer slot(s), "
                                       "{} candidate(s), {} match(es), {} path(s), {} node(s)",
                                       report.regions_scanned,
                                       report.data_regions,
                                       report.bytes_read,
                                       report.pointer_slots,
                                       report.candidate_slots,
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
            text += "; skipped {}", describe_excluded_regions(report.excluded);
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

        const std::size_t   pointer_size = config.pointer_size == 4 ? 4 : 8;
        const std::size_t   levels       = std::max<std::size_t>(1, config.levels);
        const std::int64_t  max_offset   = std::max<std::int64_t>(0, config.max_offset);
        const std::uint64_t span         = static_cast<std::uint64_t>(max_offset);
        const std::size_t   chunk_size = std::max(pointer_size, config.chunk_size - (config.chunk_size % pointer_size));

        if (module.end <= module.base)
        {
            exclude_region(report.excluded, module.base, module.end, RegionReason::empty_module);
            log::debug(log::category::scan, std::format("pointer scan of {}: {}", module.name, describe(report)));
            return scan;
        }

        std::vector<Range> base_ranges;
        std::vector<Range> writable_ranges;
        for (const process::RegionInfo& region : regions)
        {
            if (region.start >= region.end)
            {
                exclude_region(report.excluded, region.start, region.end, RegionReason::empty_module);
                continue;
            }
            if (region.readable && region.writable)
            {
                add_range(writable_ranges, region.start, region.end);
            }
            if (is_data_region(module, region))
            {
                add_range(base_ranges, std::max(region.start, module.base), std::min(region.end, module.end));
            }
        }
        merge_ranges(writable_ranges);
        merge_ranges(base_ranges);
        if (base_ranges.empty())
        {
            exclude_region(report.excluded, module.base, module.end, RegionReason::not_readable);
            log::debug(log::category::scan, std::format("pointer scan of {}: {}", module.name, describe(report)));
            return scan;
        }
        report.data_regions    = base_ranges.size();
        report.regions_scanned = writable_ranges.size();

        // A matching value is either within range of the target (the final
        // level) or within range of a writable slot address (an intermediate
        // level). Anything else can never be part of a path.
        std::vector<Range> interesting;
        add_range(interesting,
                  subtract_saturated(target, span),
                  add_saturated(target, span) == std::numeric_limits<std::uint64_t>::max()
                      ? std::numeric_limits<std::uint64_t>::max()
                      : add_saturated(target, span) + 1);
        for (const Range& range : writable_ranges)
        {
            add_expanded_range(interesting, range, span);
        }
        merge_ranges(interesting);

        std::vector<Chunk> tasks;
        std::size_t        total_slots = 0;
        for (const Range& range : writable_ranges)
        {
            std::uint64_t cursor = align_up(range.start, pointer_size);
            while (cursor + pointer_size <= range.end)
            {
                std::size_t want = static_cast<std::size_t>(range.end - cursor);
                want             = std::min(want, chunk_size);
                want -= want % pointer_size;
                if (want == 0)
                {
                    break;
                }
                tasks.push_back(Chunk {.start = cursor, .end = cursor + want});
                total_slots += want / pointer_size;
                cursor += want;
            }
        }

        const std::size_t requested_threads =
            config.threads == 0 ? std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, kMaxThreads)
                                : std::clamp<std::size_t>(config.threads, 1, kMaxThreads);
        const std::size_t thread_count =
            std::min<std::size_t>(requested_threads, std::max<std::size_t>(tasks.size(), 1));
        const std::size_t reserve_hint = std::min<std::size_t>(total_slots, kReserveHintLimit) / thread_count + 1;

        std::vector<Shard>       shards(thread_count);
        std::atomic<std::size_t> next_task {0};
        const auto               index_worker = [&](std::size_t worker)
        {
            Shard&                 shard = shards[worker];
            std::vector<std::byte> buffer(chunk_size);
            shard.entries.reserve(reserve_hint);

            for (;;)
            {
                const std::size_t task_index = next_task.fetch_add(1, std::memory_order_relaxed);
                if (task_index >= tasks.size())
                {
                    break;
                }
                const Chunk       task = tasks[task_index];
                const std::size_t want = static_cast<std::size_t>(task.end - task.start);

                std::size_t got = 0;
                if (const auto chunk = read(task.start, std::span<std::byte>(buffer.data(), want)); chunk.has_value())
                {
                    got = std::min<std::size_t>(*chunk, want);
                }
                if (got == 0)
                {
                    ++shard.read_failures;
                    log::debug(log::category::scan,
                               std::format("pointer scan of {}: unreadable chunk at {:#x}", module.name, task.start));
                    continue;
                }
                shard.bytes_read += got;

                const std::size_t count = got / pointer_size;
                shard.slots += count;
                for (std::size_t offset = 0; offset < count * pointer_size; offset += pointer_size)
                {
                    const std::uint64_t value =
                        decode_pointer(std::span<const std::byte>(buffer.data() + offset, pointer_size));
                    if (value == 0 || !in_ranges(interesting, value))
                    {
                        continue;
                    }
                    shard.entries.push_back(IndexedPointer {.value = value, .slot = task.start + offset});
                }
            }
        };

        if (thread_count == 1)
        {
            index_worker(0);
        }
        else
        {
            std::vector<std::jthread> pool;
            pool.reserve(thread_count);
            for (std::size_t worker = 0; worker < thread_count; ++worker)
            {
                pool.emplace_back(index_worker, worker);
            }
        }

        std::vector<IndexedPointer> index;
        std::size_t                 candidate_count = 0;
        for (const Shard& shard : shards)
        {
            candidate_count += shard.entries.size();
            report.bytes_read += shard.bytes_read;
            report.pointer_slots += shard.slots;
            report.read_failures += shard.read_failures;
        }
        index.reserve(candidate_count);
        for (Shard& shard : shards)
        {
            index.insert(index.end(),
                         std::make_move_iterator(shard.entries.begin()),
                         std::make_move_iterator(shard.entries.end()));
            shard.entries.clear();
            shard.entries.shrink_to_fit();
        }
        report.candidate_slots = index.size();
        radix_sort(index);

        const auto matching = [&](std::uint64_t current)
        {
            const std::uint64_t high = add_saturated(current, span);
            const std::uint64_t low  = config.allow_negative ? subtract_saturated(current, span) : current;
            return std::pair {std::ranges::lower_bound(index, low, {}, &IndexedPointer::value),
                              std::ranges::upper_bound(index, high, {}, &IndexedPointer::value)};
        };

        // Expand the reverse tree level by level. A slot address that several
        // bases reach is expanded once, and its parent edges are shared by every
        // path through it.
        std::vector<Node>        nodes;
        std::vector<std::size_t> frontier;
        std::vector<std::size_t> base_nodes;
        nodes.push_back(Node {.address = target, .parents = {}});
        frontier.push_back(0);
        std::size_t edges = 0;
        bool        stop  = false;

        for (std::size_t depth = 0; depth < levels && !stop; ++depth)
        {
            const bool                                     final_level = depth + 1 >= levels;
            std::vector<std::size_t>                       next;
            std::unordered_map<std::uint64_t, std::size_t> next_index;

            for (const std::size_t parent : frontier)
            {
                if (report.visited_nodes >= config.max_nodes)
                {
                    report.truncated = true;
                    stop             = true;
                    break;
                }
                ++report.visited_nodes;

                const auto [first, last] = matching(nodes[parent].address);
                for (auto it = first; it != last; ++it)
                {
                    const IndexedPointer& entry = *it;
                    ++report.matched_slots;
                    if (edges >= config.max_edges)
                    {
                        report.truncated = true;
                        stop             = true;
                        break;
                    }
                    const bool is_base = in_ranges(base_ranges, entry.slot);
                    if (final_level && !is_base)
                    {
                        continue;
                    }

                    const int64_t offset   = static_cast<std::int64_t>(nodes[parent].address - entry.value);
                    auto [found, inserted] = next_index.try_emplace(entry.slot, nodes.size());
                    if (inserted)
                    {
                        if (nodes.size() >= config.max_nodes)
                        {
                            next_index.erase(found);
                            report.truncated = true;
                            stop             = true;
                            break;
                        }
                        const std::size_t index_of_node = nodes.size();
                        nodes.push_back(Node {.address = entry.slot, .parents = {}});
                        found = next_index.find(entry.slot);
                        next.push_back(index_of_node);
                        if (is_base)
                        {
                            base_nodes.push_back(index_of_node);
                        }
                    }
                    nodes[found->second].parents.push_back(Edge {.parent = parent, .offset = offset});
                    ++edges;
                }
                if (stop)
                {
                    break;
                }
            }
            if (stop)
            {
                break;
            }

            std::ranges::sort(next,
                              [&](std::size_t lhs, std::size_t rhs)
                              {
                                  return nodes[lhs].address < nodes[rhs].address;
                              });
            frontier = std::move(next);
            if (frontier.empty())
            {
                break;
            }
        }

        {
            std::vector<std::int64_t> offsets;
            bool                      enough = false;

            const auto reconstruct = [&](auto&& self, std::size_t node, std::uint64_t base) -> void
            {
                if (node == 0)
                {
                    PointerPath path;
                    path.base   = base;
                    path.target = target;
                    path.offsets.assign(offsets.begin(), offsets.end());
                    scan.paths.push_back(std::move(path));
                    if (scan.paths.size() >= config.max_results)
                    {
                        report.truncated = true;
                        enough           = true;
                    }
                    return;
                }
                for (const Edge& edge : nodes[node].parents)
                {
                    offsets.push_back(edge.offset);
                    self(self, edge.parent, base);
                    offsets.pop_back();
                    if (enough)
                    {
                        return;
                    }
                }
            };

            for (const std::size_t node : base_nodes)
            {
                reconstruct(reconstruct, node, nodes[node].address);
                if (enough)
                {
                    break;
                }
            }
        }

        std::ranges::sort(scan.paths, path_less);
        report.paths = scan.paths.size();
        log::debug(log::category::scan, std::format("pointer scan of {}: {}", module.name, describe(report)));
        return scan;
    }
}
