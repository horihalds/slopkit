#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "platform/linux/memory.hpp"
#include "platform/linux/ptrace.hpp"
#include "process/types.hpp"

namespace slopkit::platform
{

    // What the session does with a stop it did not ask for (ABI 1.4 debug ops).
    enum class ForeignSignalPolicy
    {
        suppress, // resume with signal 0; the debugger owns the tracee (linux-proc)
        forward,  // re-deliver the signal and keep waiting (Wine's own machinery)
    };

    // Software breakpoint slots the host allocates; the session only stores the
    // replaced byte and whether the trap is currently written.
    struct SoftwareSlot
    {
        bool          armed {};
        std::uint64_t address {};
        std::uint64_t original {};
        std::size_t   size {};
    };

    // One hardware breakpoint slot (DR0-DR3).
    struct HardwareSlot
    {
        bool          armed {};
        std::int32_t  kind {};
        std::uint64_t address {};
        std::size_t   size {1};
    };

    // One raw stack frame of the frame-pointer walk.
    struct DebugFrame
    {
        std::uint64_t pc {};
        std::uint64_t frame_pointer {};
    };

    inline constexpr std::size_t kSoftwareSlotCount = 64;
    inline constexpr std::size_t kHardwareSlotCount = 4;

    // The opt-in ptrace debug session shared by the bundled plugins. It owns the
    // seized thread group, the breakpoint slot tables and the wait loop; a
    // plugin only supplies the session's process id, its cached memory access and
    // the policy for the target's own signals.
    class DebugSession
    {
    public:
        DebugSession(process::ProcessId pid, MemAccess& memory, ForeignSignalPolicy policy);
        DebugSession(const DebugSession&)            = delete;
        DebugSession& operator=(const DebugSession&) = delete;

        [[nodiscard]] bool               attached() const noexcept;
        [[nodiscard]] process::ProcessId leader() const noexcept;

        // Seizes and stops every thread of the process and returns the
        // thread-group leader. Idempotent while the session is open.
        [[nodiscard]] std::expected<process::ProcessId, process::AccessError> attach();
        // Restores every replaced byte, clears the debug registers and detaches
        // every thread. Idempotent.
        [[nodiscard]] std::expected<void, process::AccessError>               detach();

        // The single-thread window the allocation path borrows: it seizes and
        // stops exactly `tid` and leaves every other thread of the target
        // running. Refused while any window is already open, and the group
        // attach above is refused while a thread window is open.
        [[nodiscard]] std::expected<void, process::AccessError> attach_thread(process::ProcessId tid);
        // Hands one borrowed thread back: no debug register is touched (this
        // window never programs any) and a signal the thread stopped only to
        // hand over is delivered. Idempotent for a tid it does not hold.
        [[nodiscard]] std::expected<void, process::AccessError> detach_thread(process::ProcessId tid);
        // The threads the current window holds: one donor for a thread window,
        // the whole thread group for a debug window.
        [[nodiscard]] std::vector<process::ProcessId>           attached_threads() const;

        // Runs the group (or steps over the armed trap at `resume_address`) and
        // returns the first stop the debugger should report.
        [[nodiscard]] std::expected<StopStatus, process::AccessError> resume(std::uint64_t resume_address);
        // Single-steps one thread and returns the stop it reports.
        [[nodiscard]] std::expected<StopStatus, process::AccessError> step(process::ProcessId tid);
        // Resumes one thread with a syscall resume and returns the stop it
        // reports, without the foreign-stop loop `step` runs: a caller with a
        // policy of its own (the plugin allocation window) owns that.
        [[nodiscard]] std::expected<StopStatus, process::AccessError> step_syscall(process::ProcessId tid);
        // Resumes one thread with a syscall resume, delivering `signal` to its
        // own handler (0 resumes it plainly) and returning without waiting.
        [[nodiscard]] std::expected<void, process::AccessError> continue_thread(process::ProcessId tid, int signal);
        // A stop already waiting on one thread, or `std::nullopt` when it is
        // running on: the non-blocking sibling of the wait `ensure_stopped` does.
        [[nodiscard]] std::expected<std::optional<StopStatus>, process::AccessError> poll_stop(process::ProcessId tid);
        // Asks a running thread to stop without blocking.
        [[nodiscard]] std::expected<void, process::AccessError>                      interrupt(process::ProcessId tid);

        [[nodiscard]] std::expected<Registers, process::AccessError> registers(process::ProcessId tid);
        [[nodiscard]] std::expected<void, process::AccessError>
        set_register(process::ProcessId tid, std::string_view name, std::uint64_t value);

        // Arms (`insert`) or disarms a software trap in a host-allocated slot.
        [[nodiscard]] std::expected<void, process::AccessError>
        arm_software(std::uint32_t slot, std::uint64_t address, bool insert);
        // Arms or disarms a hardware breakpoint in DR slot 0-3.
        [[nodiscard]] std::expected<void, process::AccessError>
        arm_hardware(std::uint32_t slot, std::int32_t kind, std::uint64_t address, std::size_t size, bool insert);

        [[nodiscard]] std::expected<std::vector<DebugFrame>, process::AccessError> backtrace(process::ProcessId tid);

        // True when the byte at `address` is the software-trap int3; the plugin
        // uses it to tell a software trap from a plain single step.
        [[nodiscard]] bool holds_int3(std::uint64_t address);

    private:
        // Programs DR0-DR3/DR7 on every seized thread from the slot table.
        [[nodiscard]] std::expected<void, process::AccessError> apply_hardware();
        // Leaves `tid` in a ptrace stop, so the debug registers, the registers
        // and DETACH reach it.
        [[nodiscard]] std::expected<void, process::AccessError> ensure_stopped(process::ProcessId tid);

        mutable std::mutex                           mutex_;
        std::vector<process::ProcessId>              tids_;
        std::unordered_set<process::ProcessId>       requested_stops_; // our own SIGSTOP
        std::array<SoftwareSlot, kSoftwareSlotCount> software_ {};
        std::array<HardwareSlot, kHardwareSlotCount> hardware_ {};
        process::ProcessId                           pid_ {};
        process::ProcessId                           leader_ {};
        process::ProcessId                           current_ {};
        bool                                         attached_ {};
        // True while `attach` seized the whole group, false for the
        // single-thread window: the two never coexist and only the group window
        // owns the debug registers.
        bool                                         group_window_ {};
        MemAccess&                                   memory_;
        ForeignSignalPolicy                          policy_ {ForeignSignalPolicy::suppress};
    };

} // namespace slopkit::platform
