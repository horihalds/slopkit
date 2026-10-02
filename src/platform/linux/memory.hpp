#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "process/types.hpp"

namespace slopkit::platform
{

    enum class MemoryPrimitive
    {
        none,
        process_vm,
        procfs_mem,
    };

    struct MemoryTransfer
    {
        std::size_t     bytes {0};
        MemoryPrimitive primitive {MemoryPrimitive::none};
    };

    enum class MemoryError
    {
        process_not_found,
        permission_denied,
        invalid_argument,
        unmapped,
        io_error,
    };

    // Reads into `buffer` and reports how many bytes were read and which
    // primitive did it. Never attaches with ptrace. A partial read (short of
    // buffer.size()) is reported as success.
    std::expected<MemoryTransfer, MemoryError>
    read_memory(process::ProcessId pid, std::uint64_t address, std::span<std::byte> buffer);

    // Writes from `data` and reports how many bytes were written and which
    // primitive did it.
    std::expected<MemoryTransfer, MemoryError>
    write_memory(process::ProcessId pid, std::uint64_t address, std::span<const std::byte> data);

} // namespace slopkit::platform
