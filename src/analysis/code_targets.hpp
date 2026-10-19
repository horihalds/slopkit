#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "analysis/module_range.hpp"
#include "disasm/decoder.hpp"
#include "process/types.hpp"

namespace slopkit::analysis
{
    enum class CodeTargetKind
    {
        call,
        jump,
        conditional_jump,
    };

    struct CodeTarget
    {
        std::uint64_t  instruction {};
        std::uint64_t  target {};
        CodeTargetKind kind {CodeTargetKind::call};
    };

    struct CodeTargetKinds
    {
        bool call {true};
        bool jump {false};
        bool conditional_jump {false};
    };

    struct CodeTargetScanConfig
    {
        std::size_t chunk_size {64 * 1024};
    };

    // Why a sweep stopped where it did: the counters of every stage plus the
    // reasons the rejected regions were dropped. Travels with the targets so an
    // empty result can explain itself.
    struct CodeTargetScanReport
    {
        std::size_t                 regions_in_module {};
        std::size_t                 code_regions {};
        std::size_t                 bytes_read {};
        std::size_t                 read_failures {};
        std::size_t                 instructions {};
        std::size_t                 branches {};
        std::size_t                 indirect_branches {};
        std::size_t                 outside_module {};
        std::size_t                 targets {};
        std::vector<ExcludedRegion> excluded;
    };

    struct CodeTargetScanResult
    {
        std::vector<CodeTarget> targets;
        CodeTargetScanReport    report;
    };

    // Decodes the module's executable regions, clamped to the module range, and
    // reports every directly encoded call/jump target inside the module that
    // matches an enabled kind. Indirect branches carry no target and are only
    // counted. `read` and `regions` describe the live target; the function is
    // target-free and runs entirely on the caller's thread.
    [[nodiscard]] CodeTargetScanResult find_code_targets(ReadIntoFn                           read,
                                                         const ModuleRange&                   module,
                                                         std::span<const process::RegionInfo> regions,
                                                         disasm::MachineMode                  mode,
                                                         CodeTargetKinds                      kinds,
                                                         const CodeTargetScanConfig&          config = {});

    // One line naming the sweep's funnel and where it stopped, for the status
    // bar and the log.
    [[nodiscard]] std::string describe(const CodeTargetScanReport& report);
}
