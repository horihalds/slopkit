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

    // Decodes the module's executable, file-backed regions and reports every
    // direct memory operand that lands on an ASCII or UTF-16LE string in one of
    // its readable, non-executable regions. `read` and `regions` describe the
    // live target; the function is target-free and runs entirely on the caller's
    // thread.
    [[nodiscard]] std::vector<StringReference> find_string_references(ReadIntoFn                           read,
                                                                      const ModuleRange&                   module,
                                                                      std::span<const process::RegionInfo> regions,
                                                                      disasm::MachineMode                  mode,
                                                                      const StringReferenceConfig&         config = {});

}
