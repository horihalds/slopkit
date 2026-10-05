#include "platform/linux/debug_session.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include "platform/linux/procfs.hpp"

namespace slopkit::platform
{

    namespace
    {
        // The ABI's hardware breakpoint kinds; the values mirror
        // slopkit_breakpoint_kind, which the platform layer does not include.
        constexpr std::int32_t kHardwareExecute   = 1;
        constexpr std::int32_t kHardwareWrite     = 2;
        constexpr std::int32_t kHardwareReadWrite = 3;

        // DR7 control bits for one slot: local-enable, R/W and LEN.
        std::uint64_t hardware_control(std::uint32_t slot, std::int32_t kind, std::size_t size)
        {
            std::uint64_t rw = 0;
            if (kind == kHardwareWrite)
            {
                rw = 1;
            }
            else if (kind == kHardwareReadWrite)
            {
                rw = 3;
            }

            std::uint64_t length = 0;
            if (size == 2)
            {
                length = 1;
            }
            else if (size == 4)
            {
                length = 3;
            }
            else if (size == 8)
            {
                length = 2;
            }
            return (std::uint64_t {1} << (2 * slot)) | (rw << (16 + 4 * slot)) | (length << (18 + 4 * slot));
        }
    } // namespace

    DebugSession::DebugSession(process::ProcessId pid, MemAccess& memory, ForeignSignalPolicy policy)
        : pid_(pid), memory_(memory), policy_(policy)
    {
    }

    bool DebugSession::attached() const noexcept
    {
        const std::lock_guard lock(mutex_);
        return attached_;
    }

    process::ProcessId DebugSession::leader() const noexcept
    {
        const std::lock_guard lock(mutex_);
        return leader_;
    }

    std::expected<process::ProcessId, process::AccessError> DebugSession::attach()
    {
        const std::lock_guard lock(mutex_);
        if (attached_)
        {
            return leader_;
        }
        if (const auto status = read_status(pid_); status && status->tracer_pid != 0)
        {
            return std::unexpected(process::AccessError::permission_denied);
        }

        std::vector<process::ProcessId> tids;
        for (const auto& thread : read_threads(pid_))
        {
            tids.push_back(thread.tid);
        }
        if (tids.empty())
        {
            tids.push_back(pid_);
        }

        std::size_t seized = 0;
        for (; seized < tids.size(); ++seized)
        {
            const auto result = platform::seize(tids[seized]);
            if (result)
            {
                continue;
            }
            if (result.error() == process::AccessError::not_found)
            {
                // The thread went away between the listing and the seize.
                tids.erase(tids.begin() + static_cast<std::ptrdiff_t>(seized));
                --seized;
                continue;
            }
            for (std::size_t i = 0; i < seized; ++i)
            {
                (void)platform::detach(tids[i]);
            }
            return std::unexpected(result.error());
        }
        if (tids.empty())
        {
            return std::unexpected(process::AccessError::not_found);
        }

        // Interrupt and wait for each thread so the whole group is stopped
        // before any breakpoint is armed.
        for (const auto tid : tids)
        {
            const std::array<process::ProcessId, 1> one {tid};
            if (const auto interrupted = platform::interrupt(tid); !interrupted)
            {
                for (const auto other : tids)
                {
                    (void)platform::detach(other);
                }
                return std::unexpected(interrupted.error());
            }
            const auto result = platform::wait(one);
            if (!result && result.error() != process::AccessError::not_found)
            {
                for (const auto other : tids)
                {
                    (void)platform::detach(other);
                }
                return std::unexpected(result.error());
            }
        }

        tids_     = std::move(tids);
        leader_   = pid_;
        current_  = pid_;
        attached_ = true;
        return leader_;
    }

    std::expected<void, process::AccessError> DebugSession::detach()
    {
        std::vector<process::ProcessId>                     tids;
        std::vector<std::pair<std::uint64_t, std::uint8_t>> restore;
        process::ProcessId                                  leader {};
        {
            const std::lock_guard lock(mutex_);
            if (!attached_)
            {
                return {};
            }
            tids   = tids_;
            leader = leader_;
            for (const auto& slot : software_)
            {
                if (slot.armed && slot.size > 0)
                {
                    restore.emplace_back(slot.address, static_cast<std::uint8_t>(slot.original & 0xFF));
                }
            }
            attached_ = false;
        }

        // Restore the original bytes so no int3 is left in the target, then
        // clear the slots and detach every thread.
        for (const auto& [address, original] : restore)
        {
            const std::array<std::byte, 1> bytes {static_cast<std::byte>(original)};
            (void)platform::write_bytes(leader, address, bytes);
        }

        std::expected<void, process::AccessError> result {};
        for (const auto tid : tids)
        {
            (void)platform::clear_debug_registers(tid);
            const auto detached = platform::detach(tid);
            if (!detached && result.has_value())
            {
                result = std::unexpected(detached.error());
            }
        }

        {
            const std::lock_guard lock(mutex_);
            tids_.clear();
            requested_stops_.clear();
            software_ = {};
            hardware_ = {};
        }
        return result;
    }

    std::expected<StopStatus, process::AccessError> DebugSession::resume(std::uint64_t resume_address)
    {
        std::vector<process::ProcessId> tids;
        process::ProcessId              current {};
        bool                            have_trap     = false;
        std::uint64_t                   trap_original = 0;
        {
            const std::lock_guard lock(mutex_);
            if (!attached_)
            {
                return std::unexpected(process::AccessError::invalid_argument);
            }
            tids    = tids_;
            current = current_;
            if (resume_address != 0)
            {
                for (const auto& slot : software_)
                {
                    if (slot.armed && slot.address == resume_address)
                    {
                        have_trap     = true;
                        trap_original = slot.original;
                        break;
                    }
                }
            }
        }

        std::expected<StopStatus, process::AccessError> stop = std::unexpected(process::AccessError::internal);

        if (resume_address != 0 && have_trap)
        {
            // Step over the trap: rewind RIP (an int3 leaves it one byte past
            // the trap), restore the original byte, single-step it and write the
            // int3 back, leaving the target stopped.
            if (const auto rewound = platform::set_register(current, "RIP", resume_address); !rewound)
            {
                return std::unexpected(rewound.error());
            }
            const std::array<std::byte, 1> original {static_cast<std::byte>(trap_original & 0xFF)};
            if (const auto restored = platform::write_bytes(current, resume_address, original); !restored)
            {
                return std::unexpected(restored.error());
            }
            const std::array<process::ProcessId, 1> one {current};
            int                                     pending = 0;
            for (;;)
            {
                if (const auto stepped = platform::single_step(current, pending); !stepped)
                {
                    const std::array<std::byte, 1> trap {std::byte {0xCC}};
                    (void)platform::write_bytes(current, resume_address, trap);
                    return std::unexpected(stepped.error());
                }
                stop = platform::wait(one);
                if (!stop)
                {
                    const std::array<std::byte, 1> trap {std::byte {0xCC}};
                    (void)platform::write_bytes(current, resume_address, trap);
                    return std::unexpected(stop.error());
                }
                if (policy_ == ForeignSignalPolicy::forward && stop->reason == StopReason::signal_stop)
                {
                    // The target's own handler must still run; re-send the signal
                    // with the next single step so the step is not lost.
                    pending = stop->signal;
                    continue;
                }
                break;
            }
            const std::array<std::byte, 1> trap {std::byte {0xCC}};
            (void)platform::write_bytes(current, resume_address, trap);
        }
        else
        {
            if (const auto hardware = apply_hardware(); !hardware)
            {
                return std::unexpected(hardware.error());
            }
            for (const auto tid : tids)
            {
                (void)platform::cont(tid);
            }
            for (;;)
            {
                stop = platform::wait(tids);
                if (!stop)
                {
                    return std::unexpected(stop.error());
                }
                if (policy_ != ForeignSignalPolicy::forward)
                {
                    break;
                }
                if (stop->reason == StopReason::interrupt)
                {
                    // The debugger's own SIGSTOP is a stop to report; a
                    // job-control stop the target's own runtime produced is
                    // foreign, so resume it without re-delivering the signal.
                    bool ours = false;
                    {
                        const std::lock_guard lock(mutex_);
                        ours = requested_stops_.erase(stop->tid) != 0;
                    }
                    if (ours)
                    {
                        break;
                    }
                    if (const auto resumed = platform::cont(stop->tid, 0); !resumed)
                    {
                        return std::unexpected(resumed.error());
                    }
                    continue;
                }
                if (stop->reason != StopReason::signal_stop)
                {
                    break;
                }
                // Re-deliver the target's own signal and keep waiting.
                const auto signal = stop->signal;
                if (const auto resumed = platform::cont(stop->tid, signal); !resumed)
                {
                    return std::unexpected(resumed.error());
                }
            }
        }

        {
            const std::lock_guard lock(mutex_);
            current_ = stop->tid;
        }
        return stop;
    }

    std::expected<StopStatus, process::AccessError> DebugSession::step(process::ProcessId tid)
    {
        {
            const std::lock_guard lock(mutex_);
            if (!attached_)
            {
                return std::unexpected(process::AccessError::invalid_argument);
            }
        }

        const std::array<process::ProcessId, 1>         one {tid};
        int                                             pending = 0;
        std::expected<StopStatus, process::AccessError> stop    = std::unexpected(process::AccessError::internal);
        for (;;)
        {
            if (const auto stepped = platform::single_step(tid, pending); !stepped)
            {
                return std::unexpected(stepped.error());
            }
            stop = platform::wait(one);
            if (!stop)
            {
                return std::unexpected(stop.error());
            }
            if (policy_ == ForeignSignalPolicy::forward && stop->reason == StopReason::signal_stop)
            {
                pending = stop->signal;
                continue;
            }
            break;
        }

        {
            const std::lock_guard lock(mutex_);
            current_ = stop->tid;
        }
        return stop;
    }

    std::expected<void, process::AccessError> DebugSession::interrupt(process::ProcessId tid)
    {
        {
            const std::lock_guard lock(mutex_);
            if (!attached_)
            {
                return std::unexpected(process::AccessError::invalid_argument);
            }
            requested_stops_.insert(tid);
        }

        // Not a ptrace call: SIGSTOP is delivered to the tracee so it can be
        // sent from the worker's interrupt thread while the ptrace job thread is
        // blocked inside resume()'s wait.
        const auto stopped = platform::stop_thread(pid_, tid);
        if (!stopped)
        {
            const std::lock_guard lock(mutex_);
            requested_stops_.erase(tid);
            if (stopped.error() != process::AccessError::not_found)
            {
                return std::unexpected(stopped.error());
            }
        }
        return {};
    }

    std::expected<Registers, process::AccessError> DebugSession::registers(process::ProcessId tid)
    {
        return platform::get_registers(tid);
    }

    std::expected<void, process::AccessError>
    DebugSession::set_register(process::ProcessId tid, std::string_view name, std::uint64_t value)
    {
        return platform::set_register(tid, name, value);
    }

    std::expected<void, process::AccessError>
    DebugSession::arm_software(std::uint32_t slot, std::uint64_t address, bool insert)
    {
        if (slot >= kSoftwareSlotCount)
        {
            return std::unexpected(process::AccessError::invalid_argument);
        }

        const std::lock_guard lock(mutex_);
        if (!attached_)
        {
            return std::unexpected(process::AccessError::invalid_argument);
        }
        auto& record = software_[slot];

        if (!insert)
        {
            if (record.armed && record.address == address)
            {
                const std::array<std::byte, 1> original {static_cast<std::byte>(record.original & 0xFF)};
                if (const auto restored = platform::write_bytes(leader_, address, original); !restored)
                {
                    return std::unexpected(restored.error());
                }
                record.armed = false;
            }
            return {};
        }

        if (record.armed && record.address == address)
        {
            return {};
        }
        for (std::size_t i = 0; i < kSoftwareSlotCount; ++i)
        {
            if (i != slot && software_[i].armed && software_[i].address == address)
            {
                return std::unexpected(process::AccessError::invalid_argument);
            }
        }

        const auto original = platform::read_bytes(leader_, address, 1);
        if (!original)
        {
            return std::unexpected(original.error());
        }
        if (original->empty())
        {
            return std::unexpected(process::AccessError::io_error);
        }
        const std::array<std::byte, 1> trap {std::byte {0xCC}};
        if (const auto written = platform::write_bytes(leader_, address, trap); !written)
        {
            return std::unexpected(written.error());
        }
        record.armed    = true;
        record.address  = address;
        record.original = std::to_integer<std::uint8_t>((*original)[0]);
        record.size     = 1;
        return {};
    }

    std::expected<void, process::AccessError> DebugSession::arm_hardware(
        std::uint32_t slot, std::int32_t kind, std::uint64_t address, std::size_t size, bool insert)
    {
        if (slot >= kHardwareSlotCount)
        {
            return std::unexpected(process::AccessError::invalid_argument);
        }
        if (kind < kHardwareExecute || kind > kHardwareReadWrite)
        {
            return std::unexpected(process::AccessError::invalid_argument);
        }

        std::size_t effective = size;
        if (kind == kHardwareExecute)
        {
            effective = 1;
        }
        else if (effective != 1 && effective != 2 && effective != 4 && effective != 8)
        {
            return std::unexpected(process::AccessError::invalid_argument);
        }

        {
            const std::lock_guard lock(mutex_);
            if (!attached_)
            {
                return std::unexpected(process::AccessError::invalid_argument);
            }
            if (insert)
            {
                hardware_[slot] = HardwareSlot {true, kind, address, effective};
            }
            else
            {
                // The address is kept so a later apply can rewrite the slot and
                // have the kernel recompute it as disabled.
                hardware_[slot].armed = false;
            }
        }
        return apply_hardware();
    }

    std::expected<std::vector<DebugFrame>, process::AccessError> DebugSession::backtrace(process::ProcessId tid)
    {
        const auto registers = platform::get_registers(tid);
        if (!registers)
        {
            return std::unexpected(registers.error());
        }

        std::vector<DebugFrame> frames;
        frames.push_back(DebugFrame {registers->rip, registers->rbp});

        constexpr std::size_t     kMaxFrames = 64;
        std::array<std::byte, 16> buffer {};
        std::uint64_t             frame_pointer = registers->rbp;
        while (frame_pointer != 0 && frames.size() < kMaxFrames)
        {
            const auto read = memory_.read(frame_pointer, buffer);
            if (!read || read->bytes < buffer.size())
            {
                break;
            }
            std::uint64_t saved  = 0;
            std::uint64_t caller = 0;
            for (std::size_t i = 0; i < 8; ++i)
            {
                saved |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(buffer[i])) << (8 * i);
                caller |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(buffer[8 + i])) << (8 * i);
            }
            // A frame pointer must grow towards the caller; a null or
            // non-advancing one ends the walk.
            if (caller == 0 || saved <= frame_pointer)
            {
                break;
            }
            frames.push_back(DebugFrame {caller, saved});
            frame_pointer = saved;
        }
        return frames;
    }

    bool DebugSession::holds_int3(std::uint64_t address)
    {
        std::array<std::byte, 1> byte {};
        const auto               read = memory_.read(address, byte);
        return read && read->bytes == byte.size() && byte[0] == std::byte {0xCC};
    }

    std::expected<void, process::AccessError> DebugSession::apply_hardware()
    {
        std::vector<process::ProcessId>              tids;
        std::array<HardwareSlot, kHardwareSlotCount> hardware;
        {
            const std::lock_guard lock(mutex_);
            tids     = tids_;
            hardware = hardware_;
        }

        for (const auto tid : tids)
        {
            // A slot's perf attributes are derived from the DR7 value present at
            // the moment its address is written, and the kernel refuses to
            // modify an enabled breakpoint. So: disable through DR7, rewrite
            // every known address (which now lands disabled), publish the wanted
            // DR7, then rewrite the armed addresses so they pick up the enable
            // and type bits.
            if (const auto disabled = platform::set_debug_register(tid, 7, 0); !disabled)
            {
                return std::unexpected(disabled.error());
            }
            for (std::uint32_t slot = 0; slot < kHardwareSlotCount; ++slot)
            {
                if (hardware[slot].address == 0)
                {
                    continue;
                }
                if (const auto written = platform::set_debug_register(tid, slot, hardware[slot].address); !written)
                {
                    return std::unexpected(written.error());
                }
            }

            std::uint64_t control = 0;
            for (std::uint32_t slot = 0; slot < kHardwareSlotCount; ++slot)
            {
                if (hardware[slot].armed)
                {
                    control |= hardware_control(slot, hardware[slot].kind, hardware[slot].size);
                }
            }
            if (const auto written = platform::set_debug_register(tid, 7, control); !written)
            {
                return std::unexpected(written.error());
            }
            for (std::uint32_t slot = 0; slot < kHardwareSlotCount; ++slot)
            {
                if (!hardware[slot].armed)
                {
                    continue;
                }
                if (const auto written = platform::set_debug_register(tid, slot, hardware[slot].address); !written)
                {
                    return std::unexpected(written.error());
                }
            }
        }
        return {};
    }

} // namespace slopkit::platform
