#include "plugins/support/allocate.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "platform/linux/procfs.hpp"
#include "platform/linux/ptrace.hpp"
#include "plugin/plugin_api.h"

namespace slopkit::plugins::support
{
    namespace
    {
        constexpr std::uint64_t kProtRead          = 0x1;
        constexpr std::uint64_t kProtWrite         = 0x2;
        constexpr std::uint64_t kProtExec          = 0x4;
        constexpr std::uint64_t kMapPrivate        = 0x02;
        constexpr std::uint64_t kMapAnonymous      = 0x20;
        // Linux 4.17+: fail instead of silently ignoring a hint that is taken.
        constexpr std::uint64_t kMapFixedNoreplace = 0x100000;

        // How much executable memory is scanned for a syscall instruction before
        // giving up. A bound keeps a huge executable mapping from stalling the
        // run; every real image exposes one long before this.
        constexpr std::size_t kMaxGadgetScanBytes = 64u * 1024u * 1024u;
        constexpr std::size_t kGadgetChunkBytes   = 4096;

        std::string errno_text(int error)
        {
            return std::generic_category().message(error) + " (errno " + std::to_string(error) + ")";
        }

        std::int32_t status_for_errno(int error)
        {
            switch (error)
            {
            case ENOSYS:
                return SLOPKIT_ERR_UNSUPPORTED;
            case EPERM:
            case EACCES:
                return SLOPKIT_ERR_PERMISSION_DENIED;
            case EINVAL:
                return SLOPKIT_ERR_INVALID_ARGUMENT;
            default:
                return SLOPKIT_ERR_IO;
            }
        }

        AllocationError failure(std::int32_t code, std::string message)
        {
            return AllocationError {code, std::move(message)};
        }

        std::uint64_t page_size()
        {
            const long value = ::sysconf(_SC_PAGESIZE);
            return value > 0 ? static_cast<std::uint64_t>(value) : 4096;
        }

        // Finds the first `0F 05` (SYSCALL) byte pair in an executable region,
        // scanning ascending so the run is deterministic. The exact byte is a
        // valid syscall boundary when RIP points straight at it, which is how the
        // remote call uses it.
        std::expected<std::uint64_t, AllocationError> find_syscall_gadget(Session& session)
        {
            std::vector<std::byte> buffer(kGadgetChunkBytes);
            std::size_t            scanned = 0;

            for (const platform::MappedRegion& region : platform::read_maps(session.pid))
            {
                if (!region.executable)
                {
                    continue;
                }
                for (std::uint64_t address = region.start; address + 1 < region.end && scanned < kMaxGadgetScanBytes;
                     address += kGadgetChunkBytes)
                {
                    const std::size_t size =
                        static_cast<std::size_t>(std::min<std::uint64_t>(kGadgetChunkBytes, region.end - address));
                    const auto read = session.mem.read(address, std::span<std::byte>(buffer.data(), size));
                    if (read.has_value() && read->bytes >= 2)
                    {
                        const std::size_t usable = std::min<std::size_t>(read->bytes, size);
                        for (std::size_t i = 0; i + 1 < usable; ++i)
                        {
                            if (std::to_integer<std::uint8_t>(buffer[i]) == 0x0F
                                && std::to_integer<std::uint8_t>(buffer[i + 1]) == 0x05)
                            {
                                return address + i;
                            }
                        }
                    }
                    scanned += size;
                }
            }
            return std::unexpected(
                failure(SLOPKIT_ERR_UNSUPPORTED, "no syscall instruction found in the target's executable memory"));
        }

        void restore_registers(platform::DebugSession& debug, process::ProcessId tid, const platform::Registers& saved)
        {
            (void)debug.set_register(tid, "RAX", saved.rax);
            (void)debug.set_register(tid, "RBX", saved.rbx);
            (void)debug.set_register(tid, "RCX", saved.rcx);
            (void)debug.set_register(tid, "RDX", saved.rdx);
            (void)debug.set_register(tid, "RSI", saved.rsi);
            (void)debug.set_register(tid, "RDI", saved.rdi);
            (void)debug.set_register(tid, "RBP", saved.rbp);
            (void)debug.set_register(tid, "RSP", saved.rsp);
            (void)debug.set_register(tid, "R8", saved.r8);
            (void)debug.set_register(tid, "R9", saved.r9);
            (void)debug.set_register(tid, "R10", saved.r10);
            (void)debug.set_register(tid, "R11", saved.r11);
            (void)debug.set_register(tid, "R12", saved.r12);
            (void)debug.set_register(tid, "R13", saved.r13);
            (void)debug.set_register(tid, "R14", saved.r14);
            (void)debug.set_register(tid, "R15", saved.r15);
            (void)debug.set_register(tid, "RFLAGS", saved.rflags);
            (void)debug.set_register(tid, "RIP", saved.rip);
        }

        // Runs one syscall inside the target. The session's debug session seizes
        // and stops the thread group, so the whole sequence is invisible to the
        // target; every saved register is put back and the group is detached
        // before returning, whether the call succeeded or failed.
        std::expected<std::uint64_t, AllocationError>
        run_remote_syscall(Session& session, std::uint64_t number, const std::array<std::uint64_t, 6>& arguments)
        {
            const auto leader_result = session.debug.attach();
            if (!leader_result)
            {
                return std::unexpected(
                    failure(SLOPKIT_ERR_PERMISSION_DENIED, "cannot stop the target to run a syscall"));
            }
            const process::ProcessId leader = *leader_result;

            const auto saved = session.debug.registers(leader);
            if (!saved)
            {
                (void)session.debug.detach();
                return std::unexpected(failure(SLOPKIT_ERR_IO, "cannot read the target's registers"));
            }

            // Restore the register file and drop the tracer on every exit path.
            struct Guard
            {
                platform::DebugSession&    debug;
                process::ProcessId         leader;
                const platform::Registers& saved;

                ~Guard()
                {
                    restore_registers(debug, leader, saved);
                    (void)debug.detach();
                }
            } guard {session.debug, leader, *saved};

            const auto gadget = find_syscall_gadget(session);
            if (!gadget)
            {
                return std::unexpected(gadget.error());
            }

            const std::array<std::pair<const char*, std::uint64_t>, 7> registers {
                {
                 {"RIP", *gadget},
                 {"RAX", number},
                 {"RDI", arguments[0]},
                 {"RSI", arguments[1]},
                 {"RDX", arguments[2]},
                 {"R10", arguments[3]},
                 {"R8", arguments[4]},
                 }
            };
            for (const auto& [name, value] : registers)
            {
                if (const auto written = session.debug.set_register(leader, name, value); !written)
                {
                    return std::unexpected(failure(SLOPKIT_ERR_IO, "cannot set up the remote syscall"));
                }
            }
            if (const auto written = session.debug.set_register(leader, "R9", arguments[5]); !written)
            {
                return std::unexpected(failure(SLOPKIT_ERR_IO, "cannot set up the remote syscall"));
            }

            const auto stop = session.debug.step(leader);
            if (!stop)
            {
                return std::unexpected(failure(SLOPKIT_ERR_IO, "the remote syscall did not complete"));
            }

            const auto after = session.debug.registers(leader);
            if (!after)
            {
                return std::unexpected(failure(SLOPKIT_ERR_IO, "cannot read the syscall result"));
            }

            const auto value = after->rax;
            if (static_cast<std::int64_t>(value) < 0)
            {
                const auto error = static_cast<int>(-static_cast<std::int64_t>(value));
                if (error > 0 && error < 4096)
                {
                    return std::unexpected(failure(status_for_errno(error), errno_text(error)));
                }
                return std::unexpected(failure(SLOPKIT_ERR_IO, "the remote syscall failed"));
            }
            return value;
        }

        std::uint64_t round_up_to_page(std::uint64_t size)
        {
            const std::uint64_t page = page_size();
            if (size > std::numeric_limits<std::uint64_t>::max() - (page - 1))
            {
                return 0;
            }
            return (size + page - 1) / page * page;
        }
    } // namespace

    void begin_allocation(Session& session)
    {
        const std::lock_guard lock(session.allocation_mutex);
        session.allocating = true;
    }

    void end_allocation(Session& session)
    {
        const std::lock_guard lock(session.allocation_mutex);
        session.allocating = false;
    }

    bool allocation_in_flight(Session& session)
    {
        const std::lock_guard lock(session.allocation_mutex);
        return session.allocating;
    }

    std::expected<std::uint64_t, AllocationError>
    allocate_memory(Session& session, std::uint64_t size, std::uint64_t near_address)
    {
        {
            const std::lock_guard lock(session.allocation_mutex);
            if (session.debug.attached())
            {
                return std::unexpected(failure(SLOPKIT_ERR_PERMISSION_DENIED, "the target is being debugged"));
            }
            session.allocating = true;
        }

        struct InFlight
        {
            Session& session;

            ~InFlight()
            {
                end_allocation(session);
            }
        } in_flight {session};

        const std::uint64_t rounded = round_up_to_page(size);
        if (rounded == 0)
        {
            return std::unexpected(failure(SLOPKIT_ERR_INVALID_ARGUMENT, "the size is too large"));
        }

        const std::uint64_t protection = kProtRead | kProtWrite | kProtExec;
        const std::uint64_t base_flags = kMapPrivate | kMapAnonymous;
        const std::uint64_t no_hint    = 0xFFFFFFFFFFFFFFFFull;

        const auto attempt = [&](std::uint64_t hint) -> std::expected<std::uint64_t, AllocationError>
        {
            const std::uint64_t                flags = base_flags | (hint != 0 ? kMapFixedNoreplace : 0);
            const std::array<std::uint64_t, 6> arguments {hint != 0 ? hint : 0, rounded, protection, flags, no_hint, 0};
            return run_remote_syscall(session, static_cast<std::uint64_t>(SYS_mmap), arguments);
        };

        auto result = attempt(near_address);
        if (!result && near_address != 0)
        {
            // A hint the kernel refuses (taken address, no MAP_FIXED_NOREPLACE)
            // never fails the call while a hint-less mapping succeeds.
            result = attempt(0);
        }
        if (!result)
        {
            return std::unexpected(result.error());
        }

        {
            const std::lock_guard lock(session.allocation_mutex);
            session.allocations[*result] = static_cast<std::size_t>(rounded);
        }
        return *result;
    }

    std::expected<void, AllocationError> free_memory(Session& session, std::uint64_t address)
    {
        std::uint64_t size = 0;
        {
            const std::lock_guard lock(session.allocation_mutex);
            if (session.debug.attached())
            {
                return std::unexpected(failure(SLOPKIT_ERR_PERMISSION_DENIED, "the target is being debugged"));
            }
            const auto found = session.allocations.find(address);
            if (found == session.allocations.end())
            {
                return std::unexpected(failure(SLOPKIT_ERR_NOT_FOUND, "not this session's allocation"));
            }
            size               = found->second;
            session.allocating = true;
        }

        struct InFlight
        {
            Session& session;

            ~InFlight()
            {
                end_allocation(session);
            }
        } in_flight {session};

        const std::array<std::uint64_t, 6> arguments {address, size, 0, 0, 0, 0};
        const auto result = run_remote_syscall(session, static_cast<std::uint64_t>(SYS_munmap), arguments);
        if (!result)
        {
            return std::unexpected(result.error());
        }

        const std::lock_guard lock(session.allocation_mutex);
        session.allocations.erase(address);
        return {};
    }

} // namespace slopkit::plugins::support
