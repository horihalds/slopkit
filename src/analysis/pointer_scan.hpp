#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "analysis/module_range.hpp"
#include "process/types.hpp"

namespace slopkit::analysis
{
    // One static pointer chain, written from the base towards the target: the
    // pointer at `base` plus `offsets[0]` addresses the next level, and the last
    // offset is applied to the pointer read one level below the base.
    struct PointerPath
    {
        std::uint64_t             base {};
        std::uint64_t             target {};
        std::vector<std::int64_t> offsets;
    };

    struct PointerScanConfig
    {
        std::size_t  levels {3};
        std::int64_t max_offset {0x1000};
        bool         allow_negative {false};
        std::size_t  max_results {500};
        std::size_t  max_nodes {1'000'000};
        std::size_t  max_edges {2'000'000};
        std::size_t  chunk_size {1024 * 1024};
        std::size_t  pointer_size {8};
        // 0 selects the automatic thread count; the scan never uses more than
        // the value scanner's eight.
        std::size_t  threads {0};
    };

    // Why a sweep stopped where it did: the counters of every stage plus the
    // reasons the rejected regions were dropped, so an empty result explains
    // itself. `truncated` means the result cap or the node budget was hit.
    struct PointerScanReport
    {
        std::size_t                 regions_scanned {};
        std::size_t                 data_regions {};
        std::size_t                 bytes_read {};
        std::size_t                 read_failures {};
        std::size_t                 pointer_slots {};
        std::size_t                 candidate_slots {};
        std::size_t                 matched_slots {};
        std::size_t                 visited_nodes {};
        std::size_t                 paths {};
        bool                        truncated {};
        std::vector<ExcludedRegion> excluded;
    };

    struct PointerScanResult
    {
        std::vector<PointerPath> paths;
        PointerScanReport        report;
    };

    // Indexes every aligned pointer-sized word of the readable, writable regions
    // and walks backwards from `target` to the configured number of levels. A
    // path is kept only when its static base slot lies in a readable, writable,
    // non-executable region that overlaps `module`. `read` and `regions`
    // describe the live target; the function is target-free and runs entirely on
    // the caller's thread.
    [[nodiscard]] PointerScanResult find_pointers(ReadIntoFn                           read,
                                                  const ModuleRange&                   module,
                                                  std::span<const process::RegionInfo> regions,
                                                  std::uint64_t                        target,
                                                  const PointerScanConfig&             config = {});

    // One line naming the sweep's funnel and where it stopped, for the status
    // bar and the log.
    [[nodiscard]] std::string describe(const PointerScanReport& report);
}
