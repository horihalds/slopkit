#include "plugins/support/allocate.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
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

#include "core/log.hpp"
#include "core/log_categories.hpp"
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

        constexpr const char* kIncompleteMessage = "the remote syscall did not complete";

        constexpr const char* kNoAddressMessage = "the target returned no address for the mapping";

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

        bool is_syscall_instruction(std::span<const std::byte> code, std::uint64_t address)
        {
            const auto decoded = disasm::decode(code, address, disasm::MachineMode::long_64);
            return decoded.has_value() && decoded->valid && decoded->length == 2 && decoded->text == "SYSCALL";
        }

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

        constexpr std::size_t kMaxDonorAttempts = 16;

        constexpr std::chrono::milliseconds kDonorRescanWindow {200};
        constexpr std::chrono::milliseconds kDonorRescanPause {10};

        std::vector<process::ProcessId> donor_candidates(process::ProcessId pid)
        {
            std::vector<process::ProcessId> running;
            std::vector<process::ProcessId> parked;
            bool                            have_leader = false;

            for (const auto& thread : platform::read_threads(pid))
            {
                if (thread.tid == pid)
                {
                    have_leader = true;
                    continue;
                }
                if (platform::read_thread_state(pid, thread.tid) == 'R')
                {
                    running.push_back(thread.tid);
                }
                else
                {
                    parked.push_back(thread.tid);
                }
            }

            if (running.empty() && parked.empty())
            {
                if (have_leader)
                {
                    running.push_back(pid);
                }
                return running;
            }
            running.insert(running.end(), parked.begin(), parked.end());
            return running;
        }

        struct BorrowCheck
        {
            bool        in_call {false};
            bool        known {true};
            std::string detail;
        };

        BorrowCheck check_borrowable(process::ProcessId pid, process::ProcessId tid)
        {
            if (const auto number = platform::read_thread_syscall(pid, tid))
            {
                if (*number < 0)
                {
                    return {};
                }
                return {
                    true, true, "thread " + std::to_string(tid) + " is inside system call " + std::to_string(*number)};
            }

            switch (platform::syscall_state(tid))
            {
            case platform::SyscallState::entry:
            case platform::SyscallState::exit:
                return {true, true, "thread " + std::to_string(tid) + " is at a system call stop"};
            case platform::SyscallState::none:
            case platform::SyscallState::unknown:
                return {false, false, "cannot tell whether thread " + std::to_string(tid) + " is in a system call"};
            }
            return {false, false, "thread " + std::to_string(tid) + " could not be classified"};
        }

        class RemoteRunner
        {
        public:
            explicit RemoteRunner(Session& session) : session_(session)
            {
                maps_             = platform::read_maps(session.pid);
                const auto gadget = find_syscall_gadget(session, maps_);
                if (!gadget)
                {
                    error_ = gadget.error();
                    return;
                }
                gadget_ = *gadget;

                if (const auto borrowed = borrow_donor(); !borrowed)
                {
                    error_ = borrowed.error();
                    return;
                }

                const auto saved = platform::read_registers_raw(donor_);
                if (!saved)
                {
                    error_ = failure(SLOPKIT_ERR_IO, "cannot read the target's registers");
                    return;
                }
                saved_raw_ = *saved;

                log::debug(log::category::script, std::format("allocation window borrowed thread {}", donor_));
                ready_ = true;
            }

            ~RemoteRunner()
            {
                (void)release();
            }

            RemoteRunner(const RemoteRunner&)            = delete;
            RemoteRunner& operator=(const RemoteRunner&) = delete;

            std::expected<void, AllocationError> release()
            {
                if (!attached_)
                {
                    return {};
                }
                attached_ = false;

                if (const auto stopped = stop_for_handback(); !stopped)
                {
                    (void)session_.debug.detach_thread(donor_);
                    return std::unexpected(stopped.error());
                }

                disarm();

                if (saved_raw_)
                {
                    if (const auto restored = platform::write_registers_raw(donor_, *saved_raw_); !restored)
                    {
                        (void)session_.debug.detach_thread(donor_);
                        return std::unexpected(failure(SLOPKIT_ERR_IO,
                                                       std::string("cannot put the target's registers back (thread ")
                                                           + std::to_string(donor_) + ")"));
                    }
                }

                if (const auto detached = session_.debug.detach_thread(donor_); !detached)
                {
                    return std::unexpected(
                        failure(SLOPKIT_ERR_IO,
                                std::string("cannot hand the target back (thread ") + std::to_string(donor_) + ")"));
                }

                if (platform::read_thread_state(session_.pid, donor_) == 't')
                {
                    return std::unexpected(
                        failure(SLOPKIT_ERR_IO,
                                std::string("the target's thread ") + std::to_string(donor_) + " is still stopped"));
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

            [[nodiscard]] const std::vector<platform::MappedRegion>& maps() const noexcept
            {
                return maps_;
            }

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

                const std::uint64_t successor = gadget_ + 2;
                const auto          started   = std::chrono::steady_clock::now();
                int                 delivered = 0;

                for (int attempt = 0; attempt < kMaxForeignStops; ++attempt)
                {
                    if (std::chrono::steady_clock::now() - started > kWindowWait)
                    {
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
                        delivered = stop->signal;
                        continue;
                    }

                    if (stop->reason == platform::StopReason::syscall && stop->syscall_entry.has_value()
                        && stop->address == successor)
                    {
                        if (*stop->syscall_entry && !entry_seen_)
                        {
                            entry_seen_ = true;
                            at_entry_   = true;
                            continue;
                        }
                        if (!*stop->syscall_entry && entry_seen_)
                        {
                            completed_        = true;
                            const auto result = read_result();
                            if (result)
                            {
                                log::debug(log::category::script,
                                           std::format("allocation window ran syscall {:#x} on thread {}: result {:#x}",
                                                       number,
                                                       donor_,
                                                       *result));
                            }
                            else
                            {
                                log::debug(log::category::script,
                                           std::format("allocation window ran syscall {:#x} on thread {}: {}",
                                                       number,
                                                       donor_,
                                                       result.error().message));
                            }
                            return result;
                        }
                    }
                }
                return std::unexpected(SyscallRefusal {0, kIncompleteMessage});
            }

        private:
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
                    if (const auto written = session_.debug.set_register(donor_, name, value); !written)
                    {
                        return std::unexpected(SyscallRefusal {0, "cannot set up the remote syscall"});
                    }
                }
                if (const auto written = session_.debug.set_register(donor_, "R9", arguments[5]); !written)
                {
                    return std::unexpected(SyscallRefusal {0, "cannot set up the remote syscall"});
                }

                planted_    = true;
                entry_seen_ = false;
                completed_  = false;
                at_entry_   = false;
                return {};
            }

            std::expected<std::uint64_t, SyscallRefusal> read_result()
            {
                const auto after = session_.debug.registers(donor_);
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

            std::expected<void, AllocationError> borrow_donor()
            {
                const auto deadline = std::chrono::steady_clock::now() + kDonorRescanWindow;

                std::string last_reason;
                bool        tried = false;
                int         scans = 0;

                for (;;)
                {
                    ++scans;

                    const auto candidates = donor_candidates(session_.pid);
                    if (candidates.empty())
                    {
                        return std::unexpected(failure(SLOPKIT_ERR_NOT_FOUND, "the target has no thread to borrow"));
                    }

                    for (std::size_t i = 0; i < candidates.size() && i < kMaxDonorAttempts; ++i)
                    {
                        const process::ProcessId tid = candidates[i];

                        if (const auto number = platform::read_thread_syscall(session_.pid, tid))
                        {
                            if (*number >= 0)
                            {
                                tried       = true;
                                last_reason = "thread " + std::to_string(tid) + " is inside system call "
                                            + std::to_string(*number);
                                if (const auto wchan = platform::read_thread_wchan(session_.pid, tid); !wchan.empty())
                                {
                                    last_reason += ", waiting in " + wchan;
                                }
                                continue;
                            }
                        }

                        const auto attached = session_.debug.attach_thread(tid);
                        if (!attached)
                        {
                            if (attached.error() == process::AccessError::permission_denied)
                            {
                                return std::unexpected(
                                    failure(SLOPKIT_ERR_PERMISSION_DENIED, "cannot stop the target to run a syscall"));
                            }
                            last_reason = "cannot stop thread " + std::to_string(tid);
                            continue;
                        }

                        tried            = true;
                        const auto check = check_borrowable(session_.pid, tid);
                        if (check.in_call)
                        {
                            last_reason = check.detail;
                            if (const auto wchan = platform::read_thread_wchan(session_.pid, tid); !wchan.empty())
                            {
                                last_reason += ", waiting in " + wchan;
                            }
                            (void)session_.debug.detach_thread(tid);
                            continue;
                        }

                        donor_    = tid;
                        attached_ = true;
                        if (!check.known)
                        {
                            log::debug(log::category::script, std::format("{}, borrowing it", check.detail));
                        }
                        if (scans > 1)
                        {
                            log::debug(log::category::script,
                                       std::format("allocation window found thread {} on scan {}", tid, scans));
                        }
                        return {};
                    }

                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        break;
                    }
                    std::this_thread::sleep_for(kDonorRescanPause);
                }

                if (tried)
                {
                    return std::unexpected(failure(SLOPKIT_ERR_PERMISSION_DENIED,
                                                   "no target thread is outside a system call after "
                                                       + std::to_string(scans) + " scans"
                                                       + (last_reason.empty() ? std::string {} : ": " + last_reason)));
                }
                return std::unexpected(
                    failure(SLOPKIT_ERR_PERMISSION_DENIED, "cannot stop the target to run a syscall"));
            }

            std::expected<platform::StopStatus, SyscallRefusal> next_stop(int signal)
            {
                const bool was_at_entry = at_entry_;
                at_entry_               = false;
                running_                = true;
                if (const auto resumed = session_.debug.continue_thread(donor_, signal); !resumed)
                {
                    running_  = false;
                    at_entry_ = was_at_entry;
                    return std::unexpected(SyscallRefusal {0, kIncompleteMessage});
                }
                return await_stop();
            }

            std::expected<platform::StopStatus, SyscallRefusal> await_stop()
            {
                const auto deadline = std::chrono::steady_clock::now() + kStopWait;
                for (;;)
                {
                    const auto stopped = session_.debug.poll_stop(donor_);
                    if (!stopped)
                    {
                        return std::unexpected(SyscallRefusal {0, kIncompleteMessage});
                    }
                    if (stopped->has_value())
                    {
                        running_ = false;
                        return **stopped;
                    }
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        return std::unexpected(SyscallRefusal {0, kIncompleteMessage});
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds {1});
                }
            }

            std::expected<void, AllocationError> stop_for_handback()
            {
                if (!running_)
                {
                    return {};
                }
                if (const auto interrupted = session_.debug.interrupt(donor_); !interrupted)
                {
                    return std::unexpected(failure(SLOPKIT_ERR_IO,
                                                   std::string("cannot stop the target to hand it back (thread ")
                                                       + std::to_string(donor_) + ")"));
                }
                const auto stopped = await_stop();
                if (!stopped)
                {
                    running_ = true;
                    return std::unexpected(failure(SLOPKIT_ERR_IO,
                                                   std::string("the target did not stop to be handed back (thread ")
                                                       + std::to_string(donor_) + ")"));
                }
                return {};
            }

            void disarm()
            {
                if (!planted_ || completed_ || !at_entry_)
                {
                    return;
                }
                (void)session_.debug.set_register(donor_, "ORIG_RAX", static_cast<std::uint64_t>(SYS_getpid));
                (void)session_.debug.set_register(donor_, "RAX", static_cast<std::uint64_t>(SYS_getpid));
                log::debug(log::category::script,
                           std::format("allocation window disarmed a planted call on thread {}", donor_));
                at_entry_ = false;
                (void)next_stop(0);
            }

            Session&                              session_;
            process::ProcessId                    donor_ {0};
            bool                                  attached_ {false};
            bool                                  ready_ {false};
            std::optional<std::vector<std::byte>> saved_raw_;
            std::uint64_t                         gadget_ {0};
            std::vector<platform::MappedRegion>   maps_;
            AllocationError                       error_ {};
            bool                                  planted_ {false};
            bool                                  entry_seen_ {false};
            bool                                  completed_ {false};
            bool                                  at_entry_ {false};
            bool                                  running_ {false};
        };

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
                return std::unexpected(released.error());
            }
            return *result;
        }

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
                const std::uint64_t floor_start = std::min(gap_end, hint_page);
                if (floor_start >= rounded)
                {
                    const std::uint64_t candidate = floor_start - rounded;
                    if (candidate >= gap_start && candidate != 0)
                    {
                        below.push_back(candidate);
                    }
                }
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
    }

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
                if (attempt.error().error != EEXIST && attempt.error().error != ENOMEM)
                {
                    return std::unexpected(as_allocation_error(attempt.error()));
                }
            }
        }

        if (!mapped)
        {
            const auto fallback = mmap_at(0, base_flags);
            if (!fallback)
            {
                return std::unexpected(as_allocation_error(fallback.error()));
            }
            address = *fallback;
        }

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

        {
            bool still_ours = false;
            for (const auto& region : platform::read_maps(session.pid))
            {
                if (region.start <= address && address + size <= region.end)
                {
                    still_ours = region.path.empty() && region.readable && region.writable && region.executable;
                    break;
                }
            }
            if (!still_ours)
            {
                return std::unexpected(
                    failure(SLOPKIT_ERR_NOT_FOUND,
                            std::format("the target no longer holds an anonymous writable executable mapping of "
                                        "{} bytes at {:#x}: not unmapping it",
                                        size,
                                        address)));
            }
        }

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

}
