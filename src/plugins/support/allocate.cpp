#include "plugins/support/allocate.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
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
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "disasm/decoder.hpp"
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

        // How many stops one remote call follows before it is refused. A target
        // signals itself constantly (Wine's runtime does) and every delivered
        // signal costs a delivery stop plus the `rt_sigreturn` stops around its
        // handler, so the bound is generous; the time bounds below are what keep
        // a signal storm from holding the window.
        constexpr int kMaxForeignStops = 256;

        // How long one resume may take to report its stop, so a target that never
        // does is refused instead of left running the planted syscall.
        constexpr std::chrono::milliseconds kStopWait {200};

        // How long the whole call may take to show its entry and then its exit. A
        // few delivered signals are harmless, but a target that never lets the
        // call run is refused rather than waited on.
        constexpr std::chrono::milliseconds kWindowWait {500};

        // A remote syscall whose stops never verified its entry and exit. Never
        // becomes an address.
        constexpr const char* kIncompleteMessage = "the remote syscall did not complete";

        // A mapping is a real address or an error: address 0 is never reported
        // as a successful allocation.
        constexpr const char* kNoAddressMessage = "the target returned no address for the mapping";

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

        // True when the bytes at `address` decode as the two-byte `syscall`
        // instruction, so a bare `0F 05` pair that is part of some other
        // instruction is not mistaken for a gadget. The remote syscall moves
        // the x86-64 register set only, so the decode always uses long mode.
        bool is_syscall_instruction(std::span<const std::byte> code, std::uint64_t address)
        {
            const auto decoded = disasm::decode(code, address, disasm::MachineMode::long_64);
            return decoded.has_value() && decoded->valid && decoded->length == 2 && decoded->text == "SYSCALL";
        }

        // Finds the first `0F 05` (SYSCALL) byte pair in a file-backed executable
        // region, scanning ascending so the run is deterministic. Only a real
        // instruction boundary is accepted: an anonymous executable mapping is
        // the target's own generated code, where the pair need not be a syscall
        // at all, and the bytes after a raw pair would otherwise be executed in
        // the target's own context.
        std::expected<std::uint64_t, AllocationError>
        find_syscall_gadget(Session& session, const std::vector<platform::MappedRegion>& maps)
        {
            std::vector<std::byte> buffer(kGadgetChunkBytes);
            std::size_t            scanned = 0;

            for (const platform::MappedRegion& region : maps)
            {
                if (!region.executable || region.path.empty())
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
                                && std::to_integer<std::uint8_t>(buffer[i + 1]) == 0x05
                                && is_syscall_instruction(std::span<const std::byte>(buffer.data() + i, usable - i),
                                                          address + i))
                            {
                                return address + i;
                            }
                        }
                    }
                    scanned += size;
                }
            }
            return std::unexpected(
                failure(SLOPKIT_ERR_UNSUPPORTED, "no syscall instruction found in the target's executable code"));
        }

        // Puts every register the window borrowed back, so the target's own
        // context is what its threads resume with. Reports the first one that did
        // not stick: a target left with a borrowed RIP is a refusal, not a
        // silent change.
        std::expected<void, AllocationError>
        restore_registers(platform::DebugSession& debug, process::ProcessId tid, const platform::Registers& saved)
        {
            const std::array<std::pair<const char*, std::uint64_t>, 18> registers {
                {
                 {"RAX", saved.rax},
                 {"RBX", saved.rbx},
                 {"RCX", saved.rcx},
                 {"RDX", saved.rdx},
                 {"RSI", saved.rsi},
                 {"RDI", saved.rdi},
                 {"RBP", saved.rbp},
                 {"RSP", saved.rsp},
                 {"R8", saved.r8},
                 {"R9", saved.r9},
                 {"R10", saved.r10},
                 {"R11", saved.r11},
                 {"R12", saved.r12},
                 {"R13", saved.r13},
                 {"R14", saved.r14},
                 {"R15", saved.r15},
                 {"RFLAGS", saved.rflags},
                 {"RIP", saved.rip},
                 }
            };
            for (const auto& [name, value] : registers)
            {
                if (const auto written = debug.set_register(tid, name, value); !written)
                {
                    return std::unexpected(failure(SLOPKIT_ERR_IO,
                                                   std::string("cannot put the target's registers back (thread ")
                                                       + std::to_string(tid) + ")"));
                }
            }
            return {};
        }

        // Seizes and stops the target's thread group once, then runs any number
        // of syscalls while it stays stopped. Sharing one attach window is what
        // makes a multi-candidate near search affordable: the attach and the
        // syscall-gadget scan dominate the cost, not the individual calls.
        //
        // Every saved register is put back, a pending signal is delivered and
        // the group is detached by `release`; the destructor is the fallback for
        // a window that returned early, whether the calls succeeded or failed.
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
                // The fallback for a window that returned early: `release` has
                // already done this on the paths that report their outcome.
                (void)release();
            }

            RemoteRunner(const RemoteRunner&)            = delete;
            RemoteRunner& operator=(const RemoteRunner&) = delete;

            // Hands the target back the way it was found and reports anything it
            // could not put right, so a window that cannot let go refuses the
            // call instead of leaving a stopped target behind.
            std::expected<void, AllocationError> release()
            {
                if (!attached_)
                {
                    return {};
                }
                attached_ = false;

                if (saved_)
                {
                    if (const auto restored = restore_registers(session_.debug, leader_, *saved_); !restored)
                    {
                        // The group still has to be let go before the refusal is
                        // reported.
                        (void)session_.debug.detach();
                        return std::unexpected(restored.error());
                    }
                }

                // Detaching delivers a signal a thread was only waiting to hand
                // over, so no pending signal of the target's is cancelled.
                if (const auto detached = session_.debug.detach(); !detached)
                {
                    return std::unexpected(
                        failure(SLOPKIT_ERR_IO,
                                std::string("cannot hand the target back (thread ") + std::to_string(leader_) + ")"));
                }

                for (const auto& thread : platform::read_threads(session_.pid))
                {
                    if (platform::read_thread_state(session_.pid, thread.tid) == 't')
                    {
                        return std::unexpected(failure(SLOPKIT_ERR_IO,
                                                       std::string("the target's thread ") + std::to_string(thread.tid)
                                                           + " is still stopped"));
                    }
                }
                return {};
            }

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

            // Runs one syscall while the group stays stopped, at the kernel's
            // syscall stops only: the instruction after the gadget (the
            // follower) never executes, so the target runs none of its own code
            // for us and no code page is touched. A negative kernel return is
            // reported as a SyscallRefusal carrying its errno.
            std::expected<std::uint64_t, SyscallRefusal> run(std::uint64_t                       number,
                                                             const std::array<std::uint64_t, 6>& arguments)
            {
                if (!ready_)
                {
                    return std::unexpected(SyscallRefusal {0, error_.message});
                }

                if (const auto planted = plant(number, arguments); !planted)
                {
                    return std::unexpected(planted.error());
                }

                // Every syscall stop of our call reports RIP at the gadget's
                // successor, which is what tells our stops from the stop an
                // interrupted syscall of the target's own produces.
                const std::uint64_t successor  = gadget_ + 2;
                const auto          started    = std::chrono::steady_clock::now();
                bool                entry_seen = false;
                int                 delivered  = 0;

                for (int attempt = 0; attempt < kMaxForeignStops; ++attempt)
                {
                    if (std::chrono::steady_clock::now() - started > kWindowWait)
                    {
                        // Even the target's own signals never made room for the
                        // call: refuse instead of holding the group stopped.
                        break;
                    }

                    const auto stop = next_stop(delivered);
                    delivered       = 0;
                    if (!stop)
                    {
                        return std::unexpected(stop.error());
                    }

                    if (stop->reason == platform::StopReason::signal_stop)
                    {
                        // The target's own signal: hand it to the handler the
                        // target installed and wait again, instead of taking its
                        // stop for the syscall's result.
                        delivered = stop->signal;
                        continue;
                    }

                    if (stop->reason == platform::StopReason::syscall && stop->syscall_entry.has_value()
                        && stop->address == successor)
                    {
                        if (*stop->syscall_entry && !entry_seen)
                        {
                            // The kernel stopped at our syscall's entry, before
                            // the syscall runs: the result is read only after
                            // this stop was seen.
                            entry_seen = true;
                            continue;
                        }
                        if (!*stop->syscall_entry && entry_seen)
                        {
                            return read_result();
                        }
                    }

                    // Anything else (a trap, an exit, an interrupt, a syscall of
                    // the target's own) is foreign: resume and wait again, under
                    // the bounded budget.
                }
                return std::unexpected(SyscallRefusal {0, kIncompleteMessage});
            }

        private:
            // Points RIP at the verified gadget with the syscall number and its
            // arguments: the state every later stop is judged against.
            std::expected<void, SyscallRefusal> plant(std::uint64_t                       number,
                                                      const std::array<std::uint64_t, 6>& arguments)
            {
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
                return {};
            }

            // The result of the verified syscall, read only at that stop. A
            // value the kernel reports as an error is a SyscallRefusal, but a
            // non-negative one is handed over as it is: for `munmap` zero is
            // success, so only the caller knows whether it is an address.
            std::expected<std::uint64_t, SyscallRefusal> read_result()
            {
                const auto after = session_.debug.registers(leader_);
                if (!after)
                {
                    return std::unexpected(SyscallRefusal {0, "cannot read the syscall result"});
                }

                const std::uint64_t value = after->rax;
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

            // Resumes the leader with a syscall resume and waits for its next
            // stop, which is given a bound of its own so a thread left running the
            // planted syscall is never waited on forever. `signal` is handed to
            // the target's own handler when non-zero, so no signal of the target's
            // is swallowed by the window.
            std::expected<platform::StopStatus, SyscallRefusal> next_stop(int signal)
            {
                if (const auto resumed = session_.debug.continue_thread(leader_, signal); !resumed)
                {
                    return std::unexpected(SyscallRefusal {0, kIncompleteMessage});
                }
                return await_stop();
            }

            // Waits for the stop the last resume asked for, bounded so a target
            // stuck in the handler a delivered signal started can never leave the
            // window (and the stopped group) waiting forever.
            std::expected<platform::StopStatus, SyscallRefusal> await_stop()
            {
                const auto deadline = std::chrono::steady_clock::now() + kStopWait;
                for (;;)
                {
                    const auto stopped = session_.debug.poll_stop(leader_);
                    if (!stopped)
                    {
                        return std::unexpected(SyscallRefusal {0, kIncompleteMessage});
                    }
                    if (stopped->has_value())
                    {
                        return **stopped;
                    }
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        return std::unexpected(SyscallRefusal {0, kIncompleteMessage});
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds {1});
                }
            }

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
            if (const auto released = runner.release(); !released)
            {
                // A window that cannot let the target go is a failure of its
                // own: the target must not be left in its stop.
                return std::unexpected(released.error());
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
            const auto                         result = runner.run(static_cast<std::uint64_t>(SYS_mmap), arguments);
            if (result && *result == 0)
            {
                // `mmap` cannot succeed at address 0, so a zero result means the
                // result was not verified: report it instead of a mapping that
                // would blame the cave's jump reach later.
                return std::unexpected(SyscallRefusal {0, kNoAddressMessage});
            }
            return result;
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

        // The window is let go before the mapping is recorded, so a target the
        // window could not hand back refuses the call.
        if (const auto released = runner.release(); !released)
        {
            return std::unexpected(released.error());
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
