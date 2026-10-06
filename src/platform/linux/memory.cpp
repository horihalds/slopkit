#include "platform/linux/memory.hpp"

#include <cerrno>
#include <cstdint>
#include <format>
#include <string>

#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>

#include "core/log.hpp"
#include "core/log_categories.hpp"

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

    MemAccess::MemAccess(process::ProcessId pid) noexcept : pid_(pid)
    {
        if (pid_ != 0)
        {
            read_fd_  = ::open(mem_path(pid_).c_str(), O_RDONLY | O_CLOEXEC);
            write_fd_ = ::open(mem_path(pid_).c_str(), O_RDWR | O_CLOEXEC);
        }
    }

    MemAccess::MemAccess(MemAccess&& other) noexcept
        : pid_(other.pid_), read_fd_(other.read_fd_), write_fd_(other.write_fd_)
    {
        other.read_fd_  = -1;
        other.write_fd_ = -1;
    }

    MemAccess& MemAccess::operator=(MemAccess&& other) noexcept
    {
        if (this != &other)
        {
            if (read_fd_ >= 0)
            {
                ::close(read_fd_);
            }
            if (write_fd_ >= 0)
            {
                ::close(write_fd_);
            }
            pid_            = other.pid_;
            read_fd_        = other.read_fd_;
            write_fd_       = other.write_fd_;
            other.read_fd_  = -1;
            other.write_fd_ = -1;
        }
        return *this;
    }

    MemAccess::~MemAccess()
    {
        if (read_fd_ >= 0)
        {
            ::close(read_fd_);
        }
        if (write_fd_ >= 0)
        {
            ::close(write_fd_);
        }
    }

    std::expected<MemoryTransfer, MemoryError> MemAccess::read(std::uint64_t address, std::span<std::byte> buffer)
    {
        if (buffer.empty())
        {
            return std::unexpected(MemoryError::invalid_argument);
        }

        ::iovec    local {buffer.data(), buffer.size()};
        ::iovec    remote {reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)), buffer.size()};
        const auto count = ::process_vm_readv(static_cast<pid_t>(pid_), &local, 1, &remote, 1, 0);
        if (count > 0)
        {
            return MemoryTransfer {static_cast<std::size_t>(count), MemoryPrimitive::process_vm};
        }
        const int vm_error = errno;

        // Fallback: the cached /proc/<pid>/mem descriptor. No ptrace, no attach.
        if (read_fd_ < 0)
        {
            log::debug(log::category::memory,
                       std::format("read failed at {:X} ({} byte(s), pid {}): process_vm errno {}",
                                   address,
                                   buffer.size(),
                                   pid_,
                                   vm_error));
            return std::unexpected(map_errno(vm_error));
        }

        const auto bytes       = ::pread(read_fd_, buffer.data(), buffer.size(), static_cast<off_t>(address));
        const int  pread_error = errno;
        if (bytes > 0)
        {
            return MemoryTransfer {static_cast<std::size_t>(bytes), MemoryPrimitive::procfs_mem};
        }
        if (bytes == 0)
        {
            log::debug(log::category::memory,
                       std::format("read failed at {:X} ({} byte(s), pid {}): unmapped", address, buffer.size(), pid_));
            return std::unexpected(MemoryError::unmapped);
        }
        log::debug(log::category::memory,
                   std::format("read failed at {:X} ({} byte(s), pid {}): process_vm errno {}, procfs_mem errno {}",
                               address,
                               buffer.size(),
                               pid_,
                               vm_error,
                               pread_error));
        return std::unexpected(map_errno(pread_error));
    }

    std::expected<MemoryTransfer, MemoryError> MemAccess::write(std::uint64_t address, std::span<const std::byte> data)
    {
        if (data.empty())
        {
            return std::unexpected(MemoryError::invalid_argument);
        }

        ::iovec    local {const_cast<std::byte*>(data.data()), data.size()};
        ::iovec    remote {reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)), data.size()};
        const auto count = ::process_vm_writev(static_cast<pid_t>(pid_), &local, 1, &remote, 1, 0);
        if (count > 0)
        {
            return MemoryTransfer {static_cast<std::size_t>(count), MemoryPrimitive::process_vm};
        }
        const int vm_error = errno;

        if (write_fd_ < 0)
        {
            log::debug(log::category::memory,
                       std::format("write failed at {:X} ({} byte(s), pid {}): process_vm errno {}",
                                   address,
                                   data.size(),
                                   pid_,
                                   vm_error));
            return std::unexpected(map_errno(vm_error));
        }

        const auto bytes        = ::pwrite(write_fd_, data.data(), data.size(), static_cast<off_t>(address));
        const int  pwrite_error = errno;
        if (bytes > 0)
        {
            return MemoryTransfer {static_cast<std::size_t>(bytes), MemoryPrimitive::procfs_mem};
        }
        if (bytes == 0)
        {
            log::debug(log::category::memory,
                       std::format("write failed at {:X} ({} byte(s), pid {}): unmapped", address, data.size(), pid_));
            return std::unexpected(MemoryError::unmapped);
        }
        log::debug(log::category::memory,
                   std::format("write failed at {:X} ({} byte(s), pid {}): process_vm errno {}, procfs_mem errno {}",
                               address,
                               data.size(),
                               pid_,
                               vm_error,
                               pwrite_error));
        return std::unexpected(map_errno(pwrite_error));
    }

    std::expected<MemoryTransfer, MemoryError>
    read_memory(process::ProcessId pid, std::uint64_t address, std::span<std::byte> buffer)
    {
        MemAccess access(pid);
        return access.read(address, buffer);
    }

    std::expected<MemoryTransfer, MemoryError>
    write_memory(process::ProcessId pid, std::uint64_t address, std::span<const std::byte> data)
    {
        MemAccess access(pid);
        return access.write(address, data);
    }

} // namespace slopkit::platform
