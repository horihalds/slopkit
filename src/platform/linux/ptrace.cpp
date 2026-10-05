#include "platform/linux/ptrace.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <csignal>
#include <elf.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

namespace slopkit::platform
{

    namespace
    {
        // The kernel's PTRACE_EVENT_STOP, and PTRACE_O_EXITKILL. Defined locally
        // so the layer does not depend on which header exposes them.
        constexpr int           kPtraceEventStop = 128;
        constexpr unsigned long kPtraceOExitKill = 0x00100000UL;
        constexpr long          kWordSize        = static_cast<long>(sizeof(std::uint64_t));

        process::AccessError classify_errno(int value)
        {
            switch (value)
            {
            case ESRCH:
                return process::AccessError::not_found;
            case EPERM:
            case EACCES:
                return process::AccessError::permission_denied;
            case EINVAL:
                return process::AccessError::invalid_argument;
            case EIO:
            case EFAULT:
            case ENOMEM:
                return process::AccessError::io_error;
            default:
                return process::AccessError::internal;
            }
        }

        std::unexpected<process::AccessError> fail(int value)
        {
            return std::unexpected(classify_errno(value));
        }

        char lower(char value)
        {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
        }

        bool iequals(std::string_view lhs, std::string_view rhs)
        {
            if (lhs.size() != rhs.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < lhs.size(); ++i)
            {
                if (lower(lhs[i]) != lower(rhs[i]))
                {
                    return false;
                }
            }
            return true;
        }

        // Reads the raw 64-bit register file; a compat task's shorter layout is
        // reported as unsupported rather than misread.
        std::expected<user_regs_struct, process::AccessError> read_raw_registers(process::ProcessId tid)
        {
            user_regs_struct raw {};
            iovec            iov {&raw, sizeof(raw)};
            if (::ptrace(PTRACE_GETREGSET, static_cast<pid_t>(tid), reinterpret_cast<void*>(NT_PRSTATUS), &iov) == -1)
            {
                return fail(errno);
            }
            if (iov.iov_len < sizeof(raw))
            {
                return std::unexpected(process::AccessError::unsupported);
            }
            return raw;
        }

        bool assign_register(user_regs_struct& raw, std::string_view name, std::uint64_t value)
        {
            const auto set = [&value, name](std::string_view candidate, auto& field)
            {
                if (!iequals(name, candidate))
                {
                    return false;
                }
                field = value;
                return true;
            };

            return set("rax", raw.rax) || set("rbx", raw.rbx) || set("rcx", raw.rcx) || set("rdx", raw.rdx)
                || set("rsi", raw.rsi) || set("rdi", raw.rdi) || set("rbp", raw.rbp) || set("rsp", raw.rsp)
                || set("r8", raw.r8) || set("r9", raw.r9) || set("r10", raw.r10) || set("r11", raw.r11)
                || set("r12", raw.r12) || set("r13", raw.r13) || set("r14", raw.r14) || set("r15", raw.r15)
                || set("rip", raw.rip) || set("rflags", raw.eflags) || set("eflags", raw.eflags);
        }

        StopStatus make_stop(process::ProcessId tid, int status)
        {
            StopStatus stop;
            stop.tid = tid;

            if (WIFEXITED(status))
            {
                stop.reason = StopReason::exited;
                return stop;
            }
            if (WIFSIGNALED(status))
            {
                stop.reason = StopReason::signalled;
                stop.signal = WTERMSIG(status);
                return stop;
            }
            if (!WIFSTOPPED(status))
            {
                stop.reason = StopReason::signalled;
                return stop;
            }

            const int signal = WSTOPSIG(status);
            stop.signal      = signal;

            // PTRACE_INTERRUPT, a delivered SIGSTOP and the debugger's own stop
            // signal all arrive as a group-stop.
            if ((status >> 8) == (signal | (kPtraceEventStop << 8)) || signal == SIGSTOP || signal == SIGTSTP)
            {
                stop.reason = StopReason::interrupt;
                return stop;
            }

            if (signal != SIGTRAP)
            {
                stop.reason = StopReason::signalled;
                if (const auto registers = get_registers(tid))
                {
                    stop.address = registers->rip;
                }
                return stop;
            }

            // A SIGTRAP is a software int3, a hardware breakpoint or a single
            // step. A software trap leaves 0xCC before RIP, which is the reliable
            // discriminator: DR6 is sticky across debug exceptions (it is only
            // refreshed by a real #DB), so it cannot identify a trap on its own.
            std::uint64_t rip = 0;
            if (const auto registers = get_registers(tid))
            {
                rip = registers->rip;
            }
            stop.address = rip;

            if (rip >= 1)
            {
                if (const auto byte = peek_data(tid, rip - 1); byte.has_value() && (*byte & 0xFF) == 0xCC)
                {
                    stop.reason       = StopReason::breakpoint;
                    stop.trap_address = rip - 1;
                    return stop;
                }
            }

            std::uint64_t dr6 = 0;
            if (const auto saved = get_debug_register(tid, 6))
            {
                dr6 = *saved;
            }
            std::uint64_t dr7 = 0;
            if (const auto saved = get_debug_register(tid, 7))
            {
                dr7 = *saved;
            }
            // A slot only counts when it is currently enabled in DR7; DR6 is
            // sticky and would otherwise name a slot the session has disarmed.
            unsigned hardware = 0;
            for (std::uint32_t slot = 0; slot < debug_slot_count; ++slot)
            {
                if ((dr6 & (1ULL << slot)) != 0 && (dr7 & (1ULL << (2 * slot))) != 0)
                {
                    hardware |= 1u << slot;
                }
            }
            if (hardware != 0)
            {
                std::uint32_t slot = 0;
                while (slot < debug_slot_count && (hardware & (1u << slot)) == 0)
                {
                    ++slot;
                }
                stop.reason     = StopReason::breakpoint;
                stop.debug_slot = slot;
                if (const auto watched = get_debug_register(tid, slot))
                {
                    stop.watch_address = *watched;
                }
                stop.trap_address = stop.watch_address != 0 ? stop.watch_address : rip;
                return stop;
            }

            stop.reason       = StopReason::single_step;
            stop.trap_address = rip;
            return stop;
        }

        long debug_register_offset(std::uint32_t slot)
        {
            return static_cast<long>(offsetof(struct user, u_debugreg) + slot * sizeof(long));
        }
    } // namespace

    std::expected<void, process::AccessError> seize(process::ProcessId tid, bool exit_kill)
    {
        const unsigned long options = exit_kill ? kPtraceOExitKill : 0;
        if (::ptrace(PTRACE_SEIZE, static_cast<pid_t>(tid), nullptr, reinterpret_cast<void*>(options)) == -1)
        {
            return fail(errno);
        }
        return {};
    }

    std::expected<void, process::AccessError> interrupt(process::ProcessId tid)
    {
        if (::ptrace(PTRACE_INTERRUPT, static_cast<pid_t>(tid), nullptr, nullptr) == -1)
        {
            return fail(errno);
        }
        return {};
    }

    std::expected<void, process::AccessError> stop_thread(process::ProcessId group, process::ProcessId tid)
    {
        if (::syscall(SYS_tgkill, static_cast<pid_t>(group), static_cast<pid_t>(tid), SIGSTOP) == -1)
        {
            return fail(errno);
        }
        return {};
    }

    std::expected<StopStatus, process::AccessError> wait(std::span<const process::ProcessId> tids)
    {
        if (tids.empty())
        {
            return std::unexpected(process::AccessError::invalid_argument);
        }

        // A single known tid can block; a set has to be polled, because Linux has
        // no "wait for any of these pids" call and waitpid(-1) would also reap
        // unrelated children of the host.
        if (tids.size() == 1)
        {
            int         status = 0;
            const pid_t result = ::waitpid(static_cast<pid_t>(tids.front()), &status, __WALL);
            if (result < 0)
            {
                return fail(errno);
            }
            return make_stop(static_cast<process::ProcessId>(result), status);
        }

        for (;;)
        {
            bool any_reachable = false;
            for (const process::ProcessId tid : tids)
            {
                int         status = 0;
                const pid_t result = ::waitpid(static_cast<pid_t>(tid), &status, __WALL | WNOHANG);
                if (result == 0)
                {
                    any_reachable = true; // still running
                    continue;
                }
                if (result < 0)
                {
                    if (errno == ECHILD)
                    {
                        continue; // already reported or not a tracee
                    }
                    return fail(errno);
                }
                return make_stop(static_cast<process::ProcessId>(result), status);
            }
            if (!any_reachable)
            {
                return std::unexpected(process::AccessError::not_found);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::expected<void, process::AccessError> cont(process::ProcessId tid, int signal)
    {
        if (::ptrace(PTRACE_CONT, static_cast<pid_t>(tid), nullptr, reinterpret_cast<void*>(static_cast<long>(signal)))
            == -1)
        {
            return fail(errno);
        }
        return {};
    }

    std::expected<void, process::AccessError> single_step(process::ProcessId tid, int signal)
    {
        if (::ptrace(
                PTRACE_SINGLESTEP, static_cast<pid_t>(tid), nullptr, reinterpret_cast<void*>(static_cast<long>(signal)))
            == -1)
        {
            return fail(errno);
        }
        return {};
    }

    std::expected<void, process::AccessError> detach(process::ProcessId tid)
    {
        if (::ptrace(PTRACE_DETACH, static_cast<pid_t>(tid), nullptr, nullptr) == -1)
        {
            return fail(errno);
        }
        return {};
    }

    std::expected<Registers, process::AccessError> get_registers(process::ProcessId tid)
    {
        const auto raw = read_raw_registers(tid);
        if (!raw)
        {
            return std::unexpected(raw.error());
        }

        Registers registers;
        registers.rax    = raw->rax;
        registers.rbx    = raw->rbx;
        registers.rcx    = raw->rcx;
        registers.rdx    = raw->rdx;
        registers.rsi    = raw->rsi;
        registers.rdi    = raw->rdi;
        registers.rbp    = raw->rbp;
        registers.rsp    = raw->rsp;
        registers.r8     = raw->r8;
        registers.r9     = raw->r9;
        registers.r10    = raw->r10;
        registers.r11    = raw->r11;
        registers.r12    = raw->r12;
        registers.r13    = raw->r13;
        registers.r14    = raw->r14;
        registers.r15    = raw->r15;
        registers.rip    = raw->rip;
        registers.rflags = raw->eflags;
        return registers;
    }

    std::expected<void, process::AccessError>
    set_register(process::ProcessId tid, std::string_view name, std::uint64_t value)
    {
        auto raw = read_raw_registers(tid);
        if (!raw)
        {
            return std::unexpected(raw.error());
        }
        if (!assign_register(*raw, name, value))
        {
            return std::unexpected(process::AccessError::invalid_argument);
        }

        iovec iov {&*raw, sizeof(*raw)};
        if (::ptrace(PTRACE_SETREGSET, static_cast<pid_t>(tid), reinterpret_cast<void*>(NT_PRSTATUS), &iov) == -1)
        {
            return fail(errno);
        }
        return {};
    }

    std::expected<std::uint64_t, process::AccessError> peek_data(process::ProcessId tid, std::uint64_t address)
    {
        errno           = 0;
        const long word = ::ptrace(PTRACE_PEEKDATA,
                                   static_cast<pid_t>(tid),
                                   reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)),
                                   nullptr);
        if (word == -1 && errno != 0)
        {
            return fail(errno);
        }
        return static_cast<std::uint64_t>(static_cast<unsigned long>(word));
    }

    std::expected<void, process::AccessError>
    poke_data(process::ProcessId tid, std::uint64_t address, std::uint64_t word)
    {
        if (::ptrace(PTRACE_POKEDATA,
                     static_cast<pid_t>(tid),
                     reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)),
                     reinterpret_cast<void*>(static_cast<std::uintptr_t>(word)))
            == -1)
        {
            return fail(errno);
        }
        return {};
    }

    std::expected<std::vector<std::byte>, process::AccessError>
    read_bytes(process::ProcessId tid, std::uint64_t address, std::size_t size)
    {
        std::vector<std::byte> bytes(size);
        std::size_t            done = 0;
        while (done < size)
        {
            const std::uint64_t current = address + done;
            const std::uint64_t aligned = current & ~static_cast<std::uint64_t>(kWordSize - 1);
            const auto          word    = peek_data(tid, aligned);
            if (!word)
            {
                return std::unexpected(word.error());
            }
            for (std::size_t i = static_cast<std::size_t>(current - aligned);
                 i < static_cast<std::size_t>(kWordSize) && done < size;
                 ++i, ++done)
            {
                bytes[done] = static_cast<std::byte>((*word >> (8 * i)) & 0xFF);
            }
        }
        return bytes;
    }

    std::expected<void, process::AccessError>
    write_bytes(process::ProcessId tid, std::uint64_t address, std::span<const std::byte> data)
    {
        std::size_t done = 0;
        while (done < data.size())
        {
            const std::uint64_t current = address + done;
            const std::uint64_t aligned = current & ~static_cast<std::uint64_t>(kWordSize - 1);
            const std::size_t   first   = static_cast<std::size_t>(current - aligned);
            const std::size_t   count =
                std::min<std::size_t>(static_cast<std::size_t>(kWordSize) - first, data.size() - done);

            const auto original = peek_data(tid, aligned);
            if (!original)
            {
                return std::unexpected(original.error());
            }

            std::uint64_t word = *original;
            for (std::size_t i = 0; i < count; ++i)
            {
                const std::size_t shift = 8 * (first + i);
                word &= ~(static_cast<std::uint64_t>(0xFF) << shift);
                word |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(data[done + i])) << shift;
            }
            if (const auto written = poke_data(tid, aligned, word); !written)
            {
                return std::unexpected(written.error());
            }
            done += count;
        }
        return {};
    }

    std::expected<void, process::AccessError>
    set_debug_register(process::ProcessId tid, std::uint32_t slot, std::uint64_t value)
    {
        if (slot >= 8)
        {
            return std::unexpected(process::AccessError::invalid_argument);
        }
        if (::ptrace(PTRACE_POKEUSER,
                     static_cast<pid_t>(tid),
                     reinterpret_cast<void*>(debug_register_offset(slot)),
                     reinterpret_cast<void*>(static_cast<std::uintptr_t>(value)))
            == -1)
        {
            return fail(errno);
        }
        return {};
    }

    std::expected<std::uint64_t, process::AccessError> get_debug_register(process::ProcessId tid, std::uint32_t slot)
    {
        if (slot >= 8)
        {
            return std::unexpected(process::AccessError::invalid_argument);
        }
        errno            = 0;
        const long value = ::ptrace(
            PTRACE_PEEKUSER, static_cast<pid_t>(tid), reinterpret_cast<void*>(debug_register_offset(slot)), nullptr);
        if (value == -1 && errno != 0)
        {
            return fail(errno);
        }
        return static_cast<std::uint64_t>(static_cast<unsigned long>(value));
    }

    std::expected<void, process::AccessError> clear_debug_registers(process::ProcessId tid)
    {
        for (std::uint32_t slot = 0; slot < debug_slot_count; ++slot)
        {
            if (const auto cleared = set_debug_register(tid, slot, 0); !cleared)
            {
                return cleared;
            }
        }
        // DR7 enables the slots; zero disables every one. DR6 is read-only.
        return set_debug_register(tid, 7, 0);
    }

} // namespace slopkit::platform
