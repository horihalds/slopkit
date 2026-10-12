#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "disasm/decoder.hpp"
#include "process/types.hpp"

namespace slopkit::analysis
{
    // Mirrors `scan::ReadIntoFn` without pulling in `process/access.hpp`, so the
    // analysis stays a leaf usable off the worker thread.
    using ReadIntoFn =
        std::function<std::expected<std::size_t, process::AccessError>(std::uint64_t, std::span<std::byte>)>;

    struct StringReference
    {
        std::uint64_t instruction {};
        std::uint64_t string_address {};
        std::string   text;
        bool          wide {};
    };

    struct StringReferenceConfig
    {
        std::size_t min_length {4};
        std::size_t max_string_bytes {256};
        std::size_t chunk_size {64 * 1024};
    };

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

    // Why a sweep stopped where it did: the counters of every stage plus the
    // reasons the rejected regions were dropped. Travels with the references so
    // an empty result can explain itself.
    struct StringScanReport
    {
        std::size_t                 regions_in_module {};
        std::size_t                 code_regions {};
        std::size_t                 bytes_read {};
        std::size_t                 read_failures {};
        std::size_t                 instructions {};
        std::size_t                 address_references {};
        std::size_t                 strings {};
        std::vector<ExcludedRegion> excluded;
    };

    struct StringScanResult
    {
        std::vector<StringReference> references;
        StringScanReport             report;
    };

    // Decodes the module's executable regions, clamped to the module range, and
    // reports every direct memory operand that lands on an ASCII or UTF-16LE
    // string at a readable address of that module. `read` and `regions` describe
    // the live target; the function is target-free and runs entirely on the
    // caller's thread.
    [[nodiscard]] StringScanResult find_string_references(ReadIntoFn                           read,
                                                          const ModuleRange&                   module,
                                                          std::span<const process::RegionInfo> regions,
                                                          disasm::MachineMode                  mode,
                                                          const StringReferenceConfig&         config = {});

    // One line naming the sweep's funnel and where it stopped, for the status
    // bar and the log.
    [[nodiscard]] std::string describe(const StringScanReport& report);

}
