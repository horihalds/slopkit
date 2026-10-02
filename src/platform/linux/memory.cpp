#include "platform/linux/memory.hpp"

#include <cerrno>
#include <cstdint>
#include <string>

#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>

namespace slopkit::platform
{

    namespace
    {
        MemoryError map_errno(int error)
        {
            switch (error)
            {
            case ESRCH:
            case ENOENT:
                return MemoryError::process_not_found;
            case EPERM:
            case EACCES:
                return MemoryError::permission_denied;
            case EFAULT:
            case EIO:
                return MemoryError::unmapped;
            case EINVAL:
                return MemoryError::invalid_argument;
            default:
                return MemoryError::io_error;
            }
        }

        std::string mem_path(process::ProcessId pid)
        {
            return "/proc/" + std::to_string(pid) + "/mem";
        }
    } // namespace

    std::expected<MemoryTransfer, MemoryError>
    read_memory(process::ProcessId pid, std::uint64_t address, std::span<std::byte> buffer)
    {
        if (buffer.empty())
        {
            return std::unexpected(MemoryError::invalid_argument);
        }

        ::iovec    local {buffer.data(), buffer.size()};
        ::iovec    remote {reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)), buffer.size()};
        const auto count = ::process_vm_readv(static_cast<pid_t>(pid), &local, 1, &remote, 1, 0);
        if (count > 0)
        {
            return MemoryTransfer {static_cast<std::size_t>(count), MemoryPrimitive::process_vm};
        }
        const int vm_error = errno;

        // Fallback: /proc/<pid>/mem. No ptrace, no attach, no stop.
        const int descriptor = ::open(mem_path(pid).c_str(), O_RDONLY | O_CLOEXEC);
        if (descriptor < 0)
        {
            return std::unexpected(map_errno(vm_error));
        }

        const auto bytes       = ::pread(descriptor, buffer.data(), buffer.size(), static_cast<off_t>(address));
        const int  pread_error = errno;
        ::close(descriptor);

        if (bytes > 0)
        {
            return MemoryTransfer {static_cast<std::size_t>(bytes), MemoryPrimitive::procfs_mem};
        }
        if (bytes == 0)
        {
            return std::unexpected(MemoryError::unmapped);
        }
        return std::unexpected(map_errno(pread_error));
    }

    std::expected<MemoryTransfer, MemoryError>
    write_memory(process::ProcessId pid, std::uint64_t address, std::span<const std::byte> data)
    {
        if (data.empty())
        {
            return std::unexpected(MemoryError::invalid_argument);
        }

        ::iovec    local {const_cast<std::byte*>(data.data()), data.size()};
        ::iovec    remote {reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)), data.size()};
        const auto count = ::process_vm_writev(static_cast<pid_t>(pid), &local, 1, &remote, 1, 0);
        if (count > 0)
        {
            return MemoryTransfer {static_cast<std::size_t>(count), MemoryPrimitive::process_vm};
        }
        const int vm_error = errno;

        const int descriptor = ::open(mem_path(pid).c_str(), O_RDWR | O_CLOEXEC);
        if (descriptor < 0)
        {
            return std::unexpected(map_errno(vm_error));
        }

        const auto bytes        = ::pwrite(descriptor, data.data(), data.size(), static_cast<off_t>(address));
        const int  pwrite_error = errno;
        ::close(descriptor);

        if (bytes > 0)
        {
            return MemoryTransfer {static_cast<std::size_t>(bytes), MemoryPrimitive::procfs_mem};
        }
        if (bytes == 0)
        {
            return std::unexpected(MemoryError::unmapped);
        }
        return std::unexpected(map_errno(pwrite_error));
    }

} // namespace slopkit::platform
