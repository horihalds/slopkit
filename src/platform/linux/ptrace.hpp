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
    enum class StopReason
    {
        breakpoint,
        single_step,
        syscall,
        interrupt,
        exited,
        signalled,
        signal_stop,
    };

    struct StopStatus
    {
        StopReason                   reason {StopReason::interrupt};
        process::ProcessId           tid {};
        int                          signal {0};
        std::uint64_t                address {};
        std::optional<bool>          syscall_entry;
        std::uint64_t                trap_address {};
        std::optional<std::uint32_t> debug_slot;
        std::uint64_t                watch_address {};
    };

    struct Registers
    {
        std::uint64_t rax {}, rbx {}, rcx {}, rdx {}, rsi {}, rdi {}, rbp {}, rsp {};
        std::uint64_t r8 {}, r9 {}, r10 {}, r11 {}, r12 {}, r13 {}, r14 {}, r15 {};
        std::uint64_t rip {}, rflags {};
    };

    inline constexpr std::uint32_t debug_slot_count = 4;

    [[nodiscard]] std::expected<void, process::AccessError> seize(process::ProcessId tid, bool exit_kill = false);

    [[nodiscard]] std::expected<void, process::AccessError> interrupt(process::ProcessId tid);

    // Stops a running traced thread by sending it SIGSTOP. Unlike the ptrace
    // calls this must be issued from the tracer thread and it works from any
    // thread, which is what lets the debugger interrupt a blocked wait.
    [[nodiscard]] std::expected<void, process::AccessError> stop_thread(process::ProcessId group,
                                                                        process::ProcessId tid);

    [[nodiscard]] std::expected<StopStatus, process::AccessError> wait(std::span<const process::ProcessId> tids);

    [[nodiscard]] std::expected<void, process::AccessError> cont(process::ProcessId tid, int signal = 0);
    [[nodiscard]] std::expected<void, process::AccessError> single_step(process::ProcessId tid, int signal = 0);
    [[nodiscard]] std::expected<void, process::AccessError> resume_syscall(process::ProcessId tid, int signal = 0);
    [[nodiscard]] std::expected<void, process::AccessError> detach(process::ProcessId tid, int signal = 0);
    [[nodiscard]] std::expected<std::optional<StopStatus>, process::AccessError> poll_stop(process::ProcessId tid);

    [[nodiscard]] std::expected<Registers, process::AccessError> get_registers(process::ProcessId tid);
    [[nodiscard]] std::expected<void, process::AccessError>
    set_register(process::ProcessId tid, std::string_view name, std::uint64_t value);

    // Where a stopped thread sits relative to a system call. `none` means the
    // stop is not a syscall stop; it does not distinguish a thread parked
    // inside a call, so `/proc/<pid>/task/<tid>/syscall` is the field that
    // decides whether a thread may be borrowed.
    enum class SyscallState
    {
        unknown,
        none,
        entry,
        exit,
    };
    [[nodiscard]] SyscallState syscall_state(process::ProcessId tid);

    [[nodiscard]] std::expected<std::vector<std::byte>, process::AccessError>
                                                            read_registers_raw(process::ProcessId tid);
    [[nodiscard]] std::expected<void, process::AccessError> write_registers_raw(process::ProcessId         tid,
                                                                                std::span<const std::byte> raw);

    [[nodiscard]] std::expected<std::uint64_t, process::AccessError> peek_data(process::ProcessId tid,
                                                                               std::uint64_t      address);
    [[nodiscard]] std::expected<void, process::AccessError>
    poke_data(process::ProcessId tid, std::uint64_t address, std::uint64_t word);

    [[nodiscard]] std::expected<std::vector<std::byte>, process::AccessError>
    read_bytes(process::ProcessId tid, std::uint64_t address, std::size_t size);
    [[nodiscard]] std::expected<void, process::AccessError>
    write_bytes(process::ProcessId tid, std::uint64_t address, std::span<const std::byte> data);

    [[nodiscard]] std::expected<void, process::AccessError>
    set_debug_register(process::ProcessId tid, std::uint32_t slot, std::uint64_t value);
    [[nodiscard]] std::expected<std::uint64_t, process::AccessError> get_debug_register(process::ProcessId tid,
                                                                                        std::uint32_t      slot);
    [[nodiscard]] std::expected<void, process::AccessError>          clear_debug_registers(process::ProcessId tid);

}
