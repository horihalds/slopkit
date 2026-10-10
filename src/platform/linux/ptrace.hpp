#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "process/types.hpp"

namespace slopkit::platform
{

    // Why a traced thread stopped, without depending on the plugin ABI.
    enum class StopReason
    {
        breakpoint,  // a software int3 trap or a hardware breakpoint
        single_step, // PTRACE_SINGLESTEP completed
        syscall,     // PTRACE_SYSCALL reached a syscall entry or exit stop
        interrupt,   // PTRACE_INTERRUPT stopped the thread group
        exited,      // the tracee exited
        signalled,   // the tracee was killed by a signal
        signal_stop, // the tracee was stopped to deliver a signal
    };

    // One waitpid result for a traced thread.
    struct StopStatus
    {
        StopReason                   reason {StopReason::interrupt};
        process::ProcessId           tid {};
        // The signal that stopped or killed the thread; 0 for a plain trap.
        int                          signal {0};
        // The stopped instruction pointer (RIP); 0 for a signal/exit stop.
        std::uint64_t                address {};
        // Set for a `syscall` stop: true at the syscall entry, false at its
        // exit. Unset when the kernel does not answer PTRACE_GET_SYSCALL_INFO.
        std::optional<bool>          syscall_entry;
        // Where a software trap lives: RIP minus the one-byte int3, else 0.
        std::uint64_t                trap_address {};
        // The DR slot that fired, for a hardware breakpoint.
        std::optional<std::uint32_t> debug_slot;
        // The watched address of a hardware breakpoint, else 0.
        std::uint64_t                watch_address {};
    };

    // The 64-bit x86-64 register file, in the order the debugger lists it.
    struct Registers
    {
        std::uint64_t rax {}, rbx {}, rcx {}, rdx {}, rsi {}, rdi {}, rbp {}, rsp {};
        std::uint64_t r8 {}, r9 {}, r10 {}, r11 {}, r12 {}, r13 {}, r14 {}, r15 {};
        std::uint64_t rip {}, rflags {};
    };

    // The ordinary hardware breakpoint slots (DR0-DR3).
    inline constexpr std::uint32_t debug_slot_count = 4;

    // Attaches to `tid` without stopping it (PTRACE_SEIZE). `exit_kill` asks the
    // kernel to kill the tracee when the tracer dies.
    [[nodiscard]] std::expected<void, process::AccessError> seize(process::ProcessId tid, bool exit_kill = false);

    // Asks a seized thread to stop at its next opportunity (PTRACE_INTERRUPT).
    [[nodiscard]] std::expected<void, process::AccessError> interrupt(process::ProcessId tid);

    // Stops a running traced thread by sending it SIGSTOP. Unlike the ptrace
    // calls this must be issued from the tracer thread and it works from any
    // thread, which is what lets the debugger interrupt a blocked wait.
    [[nodiscard]] std::expected<void, process::AccessError> stop_thread(process::ProcessId group,
                                                                        process::ProcessId tid);

    // Blocks until one of `tids` reports a stop and returns it.
    [[nodiscard]] std::expected<StopStatus, process::AccessError> wait(std::span<const process::ProcessId> tids);

    // Resumes one thread, optionally delivering `signal` (0 suppresses it).
    [[nodiscard]] std::expected<void, process::AccessError> cont(process::ProcessId tid, int signal = 0);
    // Executes exactly one instruction of one thread.
    [[nodiscard]] std::expected<void, process::AccessError> single_step(process::ProcessId tid, int signal = 0);
    // Runs one thread until its next syscall entry or exit stop, optionally
    // delivering `signal` (0 suppresses it). The thread never executes an
    // instruction of its own between the two stops.
    [[nodiscard]] std::expected<void, process::AccessError> resume_syscall(process::ProcessId tid, int signal = 0);
    // Detaches, delivering `signal` to the thread (0 lets it run freely again).
    [[nodiscard]] std::expected<void, process::AccessError> detach(process::ProcessId tid, int signal = 0);
    // A pending stop of one thread, or `std::nullopt` when it is running on.
    [[nodiscard]] std::expected<std::optional<StopStatus>, process::AccessError> poll_stop(process::ProcessId tid);

    [[nodiscard]] std::expected<Registers, process::AccessError> get_registers(process::ProcessId tid);
    // Writes one register, named the way the debugger shows it ("RAX", "rip", ...).
    [[nodiscard]] std::expected<void, process::AccessError>
    set_register(process::ProcessId tid, std::string_view name, std::uint64_t value);

    // Where a stopped thread sits relative to a system call. `none` means the
    // stop is not a syscall stop; it does not distinguish a thread parked
    // inside a call, so `/proc/<pid>/task/<tid>/syscall` is the field that
    // decides whether a thread may be borrowed.
    enum class SyscallState
    {
        unknown, // the kernel did not answer
        none,    // the stop is not a syscall entry or exit
        entry,
        exit,
    };
    [[nodiscard]] SyscallState syscall_state(process::ProcessId tid);

    // The whole `NT_PRSTATUS` register file as it is, and its wholesale
    // write-back: the fields the per-name setter does not name (orig_rax, cs,
    // ss) survive a hand-back this way.
    [[nodiscard]] std::expected<std::vector<std::byte>, process::AccessError>
                                                            read_registers_raw(process::ProcessId tid);
    [[nodiscard]] std::expected<void, process::AccessError> write_registers_raw(process::ProcessId         tid,
                                                                                std::span<const std::byte> raw);

    // Word-granular target words through PTRACE_PEEKDATA/PTRACE_POKEDATA.
    [[nodiscard]] std::expected<std::uint64_t, process::AccessError> peek_data(process::ProcessId tid,
                                                                               std::uint64_t      address);
    [[nodiscard]] std::expected<void, process::AccessError>
    poke_data(process::ProcessId tid, std::uint64_t address, std::uint64_t word);

    // Byte-granular target bytes built on the word primitives above.
    [[nodiscard]] std::expected<std::vector<std::byte>, process::AccessError>
    read_bytes(process::ProcessId tid, std::uint64_t address, std::size_t size);
    [[nodiscard]] std::expected<void, process::AccessError>
    write_bytes(process::ProcessId tid, std::uint64_t address, std::span<const std::byte> data);

    // DR0-DR3 (slots), DR6 and DR7 through PTRACE_PEEKUSER/PTRACE_POKEUSER.
    [[nodiscard]] std::expected<void, process::AccessError>
    set_debug_register(process::ProcessId tid, std::uint32_t slot, std::uint64_t value);
    [[nodiscard]] std::expected<std::uint64_t, process::AccessError> get_debug_register(process::ProcessId tid,
                                                                                        std::uint32_t      slot);
    // Zeroes DR0-DR3 and DR7 so no slot survives the session.
    [[nodiscard]] std::expected<void, process::AccessError>          clear_debug_registers(process::ProcessId tid);

} // namespace slopkit::platform
