#include "plugins/support/allocate.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <system_error>
#include <unistd.h>
#include <utility>
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

        // How many hinted pages a single allocate_memory tries before falling
        // back to a hint-less mapping. Each attempt is one remote syscall, so a
        // small bound keeps the closed-as-possible search affordable.
        constexpr std::size_t kMaxNearAttempts = 4;

        // A remote syscall that did not return a value. `error` is the target
        // kernel's positive errno when the call itself was refused, or 0 when
        // the host could not run it at all (the message says what went wrong).
        struct SyscallRefusal
        {
            int         error {0};
            std::string message;
        };

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

        AllocationError as_allocation_error(const SyscallRefusal& refusal)
        {
            if (refusal.error != 0)
            {
                return failure(status_for_errno(refusal.error), refusal.message);
            }
            return failure(SLOPKIT_ERR_IO, refusal.message);
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
        std::expected<std::uint64_t, AllocationError>
        find_syscall_gadget(Session& session, const std::vector<platform::MappedRegion>& maps)
        {
            std::vector<std::byte> buffer(kGadgetChunkBytes);
            std::size_t            scanned = 0;

            for (const platform::MappedRegion& region : maps)
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

        // Seizes and stops the target's thread group once, then runs any number
        // of syscalls while it stays stopped. Sharing one attach window is what
        // makes a multi-candidate near search affordable: the attach and the
        // syscall-gadget scan dominate the cost, not the individual calls.
        //
        // Every saved register is put back and the group is detached by the
        // destructor, whether the calls succeeded or failed.
        class RemoteRunner
        {
        public:
            explicit RemoteRunner(Session& session) : session_(session)
            {
                const auto leader_result = session.debug.attach();
                if (!leader_result)
                {
                    error_ = failure(SLOPKIT_ERR_PERMISSION_DENIED, "cannot stop the target to run a syscall");
                    return;
                }
                leader_   = *leader_result;
                attached_ = true;

                const auto saved = session.debug.registers(leader_);
                if (!saved)
                {
                    error_ = failure(SLOPKIT_ERR_IO, "cannot read the target's registers");
                    return;
                }
                saved_ = *saved;

                maps_             = platform::read_maps(session.pid);
                const auto gadget = find_syscall_gadget(session, maps_);
                if (!gadget)
                {
                    error_ = gadget.error();
                    return;
                }
                gadget_ = *gadget;
                ready_  = true;
            }

            ~RemoteRunner()
            {
                if (!attached_)
                {
                    return;
                }
                if (saved_)
                {
                    restore_registers(session_.debug, leader_, *saved_);
                }
                (void)session_.debug.detach();
            }

            RemoteRunner(const RemoteRunner&)            = delete;
            RemoteRunner& operator=(const RemoteRunner&) = delete;

            [[nodiscard]] bool ready() const noexcept
            {
                return ready_;
            }

            [[nodiscard]] const AllocationError& error() const noexcept
            {
                return error_;
            }

            // The target's mapped regions as read for the gadget scan, reused to
            // find free gaps for a near hint.
            [[nodiscard]] const std::vector<platform::MappedRegion>& maps() const noexcept
            {
                return maps_;
            }

            // Runs one syscall while the group stays stopped. A negative kernel
            // return is reported as a SyscallRefusal carrying its errno.
            std::expected<std::uint64_t, SyscallRefusal> run(std::uint64_t                       number,
                                                             const std::array<std::uint64_t, 6>& arguments)
            {
                if (!ready_)
                {
                    return std::unexpected(SyscallRefusal {0, error_.message});
                }

                const std::array<std::pair<const char*, std::uint64_t>, 7> registers {
                    {
                     {"RIP", gadget_},
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
                    if (const auto written = session_.debug.set_register(leader_, name, value); !written)
                    {
                        return std::unexpected(SyscallRefusal {0, "cannot set up the remote syscall"});
                    }
                }
                if (const auto written = session_.debug.set_register(leader_, "R9", arguments[5]); !written)
                {
                    return std::unexpected(SyscallRefusal {0, "cannot set up the remote syscall"});
                }

                const auto stop = session_.debug.step(leader_);
                if (!stop)
                {
                    return std::unexpected(SyscallRefusal {0, "the remote syscall did not complete"});
                }

                const auto after = session_.debug.registers(leader_);
                if (!after)
                {
                    return std::unexpected(SyscallRefusal {0, "cannot read the syscall result"});
                }

                const auto value = after->rax;
                if (static_cast<std::int64_t>(value) < 0)
                {
                    const auto error = static_cast<int>(-static_cast<std::int64_t>(value));
                    if (error > 0 && error < 4096)
                    {
                        return std::unexpected(SyscallRefusal {error, errno_text(error)});
                    }
                    return std::unexpected(SyscallRefusal {0, "the remote syscall failed"});
                }
                return value;
            }

        private:
            Session&                            session_;
            process::ProcessId                  leader_ {0};
            bool                                attached_ {false};
            bool                                ready_ {false};
            std::optional<platform::Registers>  saved_;
            std::uint64_t                       gadget_ {0};
            std::vector<platform::MappedRegion> maps_;
            AllocationError                     error_ {};
        };

        // Runs one syscall in a one-shot attach window; keeps free_memory's
        // munmap simple now that the runner can batch.
        std::expected<std::uint64_t, AllocationError>
        run_remote_syscall(Session& session, std::uint64_t number, const std::array<std::uint64_t, 6>& arguments)
        {
            RemoteRunner runner(session);
            if (!runner.ready())
            {
                return std::unexpected(runner.error());
            }
            const auto result = runner.run(number, arguments);
            if (!result)
            {
                return std::unexpected(as_allocation_error(result.error()));
            }
            return *result;
        }

        // Mapping addresses to try for a `near` hint, closest first: the
        // page-aligned hint itself, then the top of every free gap below the
        // hint (closest first), then the bottom of every free gap above it
        // (closest first). Only gaps that fit `rounded` bytes are considered and
        // the list is capped at kMaxNearAttempts, deduplicated.
        std::vector<std::uint64_t>
        near_candidates(const std::vector<platform::MappedRegion>& maps, std::uint64_t near, std::uint64_t rounded)
        {
            const std::uint64_t page      = page_size();
            const std::uint64_t hint_page = near / page * page;

            std::vector<std::pair<std::uint64_t, std::uint64_t>> used;
            used.reserve(maps.size());
            for (const platform::MappedRegion& region : maps)
            {
                if (region.end > region.start)
                {
                    used.emplace_back(region.start, region.end);
                }
            }
            std::sort(used.begin(), used.end());

            std::vector<std::uint64_t> below;
            std::vector<std::uint64_t> above;

            const auto consider = [&](std::uint64_t gap_start, std::uint64_t gap_end)
            {
                if (gap_end <= gap_start || gap_end - gap_start < rounded)
                {
                    return;
                }
                // The closest start at or below the hint that still fits here.
                const std::uint64_t floor_start = std::min(gap_end, hint_page);
                if (floor_start >= rounded)
                {
                    const std::uint64_t candidate = floor_start - rounded;
                    if (candidate >= gap_start && candidate != 0)
                    {
                        below.push_back(candidate);
                    }
                }
                // The closest start at or above the hint that still fits here.
                const std::uint64_t candidate = std::max(gap_start, hint_page);
                if (candidate <= gap_end - rounded && candidate >= gap_start)
                {
                    above.push_back(candidate);
                }
            };

            std::uint64_t cursor = 0;
            for (const auto& [start, end] : used)
            {
                if (start > cursor)
                {
                    consider(cursor, start);
                }
                cursor = std::max(cursor, end);
            }
            consider(cursor, std::numeric_limits<std::uint64_t>::max());

            std::sort(below.begin(), below.end(), std::greater<> {});
            std::sort(above.begin(), above.end());

            std::vector<std::uint64_t> candidates;
            candidates.reserve(kMaxNearAttempts);
            const auto add = [&](std::uint64_t address)
            {
                if (address == 0 || candidates.size() >= kMaxNearAttempts)
                {
                    return;
                }
                if (std::find(candidates.begin(), candidates.end(), address) == candidates.end())
                {
                    candidates.push_back(address);
                }
            };
            add(hint_page);
            for (const std::uint64_t address : below)
            {
                add(address);
            }
            for (const std::uint64_t address : above)
            {
                add(address);
            }
            return candidates;
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

        RemoteRunner runner(session);
        if (!runner.ready())
        {
            return std::unexpected(runner.error());
        }

        const auto mmap_at = [&](std::uint64_t hint,
                                 std::uint64_t flags) -> std::expected<std::uint64_t, SyscallRefusal>
        {
            const std::array<std::uint64_t, 6> arguments {hint, rounded, protection, flags, no_hint, 0};
            return runner.run(static_cast<std::uint64_t>(SYS_mmap), arguments);
        };

        std::uint64_t address = 0;
        bool          mapped  = false;

        if (near_address != 0)
        {
            for (const std::uint64_t candidate : near_candidates(runner.maps(), near_address, rounded))
            {
                const auto attempt = mmap_at(candidate, base_flags | kMapFixedNoreplace);
                if (attempt)
                {
                    address = *attempt;
                    mapped  = true;
                    break;
                }
                // A taken or unavailable page just moves the search on; any
                // other refusal (a permission problem, a bad argument) is the
                // caller's error.
                if (attempt.error().error != EEXIST && attempt.error().error != ENOMEM)
                {
                    return std::unexpected(as_allocation_error(attempt.error()));
                }
            }
        }

        if (!mapped)
        {
            // A hint the plugin cannot honour never fails the call while a
            // hint-less mapping succeeds.
            const auto fallback = mmap_at(0, base_flags);
            if (!fallback)
            {
                return std::unexpected(as_allocation_error(fallback.error()));
            }
            address = *fallback;
        }

        {
            const std::lock_guard lock(session.allocation_mutex);
            session.allocations[address] = static_cast<std::size_t>(rounded);
        }
        return address;
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
