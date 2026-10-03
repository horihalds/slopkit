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

    // Memory access with cached /proc/<pid>/mem descriptors. process_vm_readv /
    // process_vm_writev are tried first exactly as before, but the descriptors
    // are opened once instead of per call. pread/pwrite do not use the shared
    // file offset, so a single instance may serve several scanning threads.
    class MemAccess
    {
    public:
        explicit MemAccess(process::ProcessId pid) noexcept;
        MemAccess(const MemAccess&)            = delete;
        MemAccess& operator=(const MemAccess&) = delete;
        MemAccess(MemAccess&& other) noexcept;
        MemAccess& operator=(MemAccess&& other) noexcept;
        ~MemAccess();

        std::expected<MemoryTransfer, MemoryError> read(std::uint64_t address, std::span<std::byte> buffer);
        std::expected<MemoryTransfer, MemoryError> write(std::uint64_t address, std::span<const std::byte> data);

    private:
        process::ProcessId pid_ {};
        int                read_fd_ {-1};
        int                write_fd_ {-1};
    };

} // namespace slopkit::platform
