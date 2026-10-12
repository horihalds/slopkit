#include <catch2/catch.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <future>
#include <optional>
#include <span>
#include <thread>

#include <dlfcn.h>
#include <sched.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "platform/linux/debug_session.hpp"
#include "platform/linux/memory.hpp"
#include "platform/linux/procfs.hpp"
#include "process/types.hpp"

namespace
{
    using slopkit::platform::DebugSession;
    using slopkit::platform::ForeignSignalPolicy;
    using slopkit::platform::MemAccess;
    using slopkit::platform::StopReason;
    using slopkit::process::AccessError;
    using slopkit::process::ProcessId;

    volatile std::uint32_t g_tick       = 0;
    volatile std::uint32_t g_usr1       = 0;
    volatile std::uint32_t g_go         = 0;
    volatile std::uint32_t g_marked     = 0;
    volatile std::uint32_t g_marked_tid = 0;

    extern "C" void usr1_handler(int)
    {
        g_usr1 = g_usr1 + 1;
    }

    __attribute__((noinline)) void debug_target()
    {
        g_tick = g_tick + 1;
    }

    class ChildProcess
    {
    public:
        template<class Body>
        explicit ChildProcess(Body body)
        {
            const pid_t parent = ::getpid();
            const pid_t pid    = ::fork();
            if (pid == 0)
            {
                // CTest drains this process's output pipe until EOF; a target
                // that outlives an aborted case keeps that pipe open and would
                // park the case on the per-test timeout. Die with the test
                // process instead.
                ::prctl(PR_SET_PDEATHSIG, SIGKILL);
                if (::getppid() != parent)
                {
                    ::_exit(0);
                }
                body();
                ::_exit(0);
            }
            if (pid > 0)
            {
                pid_ = static_cast<ProcessId>(pid);
            }
        }

        ~ChildProcess()
        {
            if (pid_ != 0)
            {
                ::kill(static_cast<pid_t>(pid_), SIGKILL);
                ::waitpid(static_cast<pid_t>(pid_), nullptr, 0);
            }
        }

        ChildProcess(const ChildProcess&)            = delete;
        ChildProcess& operator=(const ChildProcess&) = delete;

        [[nodiscard]] ProcessId pid() const noexcept
        {
            return pid_;
        }

    private:
        ProcessId pid_ {};
    };

    std::optional<std::uint32_t> read_u32(MemAccess& memory, std::uint64_t address)
    {
        std::array<std::byte, 4> buffer {};
        const auto               result = memory.read(address, buffer);
        if (!result || result->bytes != buffer.size())
        {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < buffer.size(); ++i)
        {
            value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(buffer[i])) << (8 * i);
        }
        return value;
    }

    std::optional<std::byte> read_byte(MemAccess& memory, std::uint64_t address)
    {
        std::array<std::byte, 1> buffer {};
        const auto               result = memory.read(address, buffer);
        if (!result || result->bytes != buffer.size())
        {
            return std::nullopt;
        }
        return buffer[0];
    }

    // A stack for the second thread; clone() is used instead of std::thread
    // because the child is forked from a process whose allocator may be locked.
    alignas(16) std::array<char, 128 * 1024> g_thread_stack {};

    extern "C" int thread_loop(void*)
    {
        for (;;)
        {
            debug_target();
        }
        return 0;
    }

    // Publishes its own tid, then keeps counting; the debug attach is told to
    // leave this thread running, so the count must keep climbing while the rest
    // of the target is stopped.
    extern "C" int marked_loop(void*)
    {
        g_marked_tid = static_cast<std::uint32_t>(::syscall(SYS_gettid));
        for (;;)
        {
            g_marked = g_marked + 1;
        }
        return 0;
    }

    bool await_tick(MemAccess& memory, std::uint32_t above)
    {
        for (int waited = 0; waited < 2000; waited += 5)
        {
            if (const auto value = read_u32(memory, reinterpret_cast<std::uint64_t>(&g_tick)); value && *value > above)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    bool await_marked(MemAccess& memory, std::uint32_t above)
    {
        for (int waited = 0; waited < 2000; waited += 5)
        {
            if (const auto value = read_u32(memory, reinterpret_cast<std::uint64_t>(&g_marked));
                value && *value > above)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    bool await_threads(ProcessId pid, std::size_t want)
    {
        for (int waited = 0; waited < 2000; waited += 5)
        {
            if (slopkit::platform::read_threads(pid).size() >= want)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    std::uint64_t find_syscall_gadget(MemAccess& memory)
    {
        auto*      entry = static_cast<std::byte*>(::dlsym(RTLD_DEFAULT, "syscall"));
        const auto base  = reinterpret_cast<std::uint64_t>(entry);

        std::array<std::byte, 64> bytes {};
        const auto                read = memory.read(base, bytes);
        if (!read)
        {
            return 0;
        }
        for (std::size_t i = 0; i + 1 < read->bytes; ++i)
        {
            if (bytes[i] == std::byte {0x0F} && bytes[i + 1] == std::byte {0x05})
            {
                return base + i;
            }
        }
        return 0;
    }
}

TEST_CASE("debug session drives a target with forwarding", "[debug_session]")
{
    ChildProcess child(
        []
        {
            for (;;)
            {
                debug_target();
            }
        });

    MemAccess    memory(child.pid());
    DebugSession session(child.pid(), memory, ForeignSignalPolicy::forward);

    // The count proves the task has run, which is the readiness the seize needs:
    // attaching to a child that is not yet established can be refused.
    REQUIRE(await_tick(memory, 0));

    // A freshly forked task can refuse a seize for a moment, so retry briefly
    // rather than failing the case on the first attempt.
    std::expected<ProcessId, AccessError> leader = std::unexpected(AccessError::not_found);
    for (int attempt = 0; attempt < 100 && !leader.has_value(); ++attempt)
    {
        leader = session.attach();
        if (!leader)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    REQUIRE(leader.has_value());
    CHECK(*leader == child.pid());
    CHECK(session.attached());

    REQUIRE(session.attach().has_value());

    const auto target = reinterpret_cast<std::uint64_t>(&debug_target);
    const auto before = read_byte(memory, target);
    REQUIRE(before.has_value());
    REQUIRE(*before != std::byte {0xCC});

    const auto registers = session.registers(*leader);
    REQUIRE(registers.has_value());
    CHECK(registers->rip != 0);
    REQUIRE(session.set_register(*leader, "RAX", 0xDEAD'BEEF).has_value());
    const auto reread = session.registers(*leader);
    REQUIRE(reread.has_value());
    CHECK(reread->rax == 0xDEAD'BEEF);
    CHECK_FALSE(session.set_register(*leader, "NOT_A_REGISTER", 1).has_value());

    REQUIRE(session.arm_software(0, target, true).has_value());
    CHECK(read_byte(memory, target) == std::byte {0xCC});

    const auto hit = session.resume(0);
    REQUIRE(hit.has_value());
    CHECK(hit->reason == StopReason::breakpoint);
    CHECK(hit->trap_address == target);
    CHECK(hit->address == target + 1);
    CHECK_FALSE(hit->debug_slot.has_value());

    const auto stepped = session.resume(target);
    REQUIRE(stepped.has_value());
    CHECK(stepped->reason == StopReason::single_step);
    CHECK(stepped->address != target);
    CHECK(read_byte(memory, target) == std::byte {0xCC});

    const auto single = session.step(stepped->tid);
    REQUIRE(single.has_value());
    CHECK(single->reason == StopReason::single_step);
    CHECK(single->address != stepped->address);

    const auto frames = session.backtrace(single->tid);
    REQUIRE(frames.has_value());
    REQUIRE_FALSE(frames->empty());
    CHECK(frames->front().pc == single->address);

    const auto again = session.resume(0);
    REQUIRE(again.has_value());
    CHECK(again->reason == StopReason::breakpoint);
    CHECK(again->trap_address == target);

    const auto cleared = session.resume(target);
    REQUIRE(cleared.has_value());
    CHECK(cleared->reason == StopReason::single_step);
    REQUIRE(session.arm_software(0, target, false).has_value());
    CHECK(read_byte(memory, target) == *before);

    REQUIRE(session.arm_hardware(0, 1, target, 1, true).has_value());
    const auto hardware = session.resume(0);
    REQUIRE(hardware.has_value());
    CHECK(hardware->reason == StopReason::breakpoint);
    REQUIRE(hardware->debug_slot.has_value());
    CHECK(*hardware->debug_slot == 0);
    CHECK(hardware->trap_address == target);
    CHECK_FALSE(session.arm_hardware(4, 1, target, 1, true).has_value());
    REQUIRE(session.arm_hardware(0, 1, target, 1, false).has_value());

    REQUIRE(session.detach().has_value());
    CHECK(session.detach().has_value());
    CHECK(::kill(static_cast<pid_t>(child.pid()), 0) == 0);
    const auto status = slopkit::platform::read_status(child.pid());
    REQUIRE(status.has_value());
    CHECK(status->tracer_pid == 0);
}

TEST_CASE("debug session forwards the target's own signal", "[debug_session]")
{
    // The child signals itself once, then runs into the trap: the session must
    // forward the signal to the child's own handler and keep waiting.
    ChildProcess child(
        []
        {
            ::signal(SIGUSR1, usr1_handler);
            while (g_go == 0)
            {
            }
            ::raise(SIGUSR1);
            for (;;)
            {
                debug_target();
            }
        });

    MemAccess    memory(child.pid());
    DebugSession session(child.pid(), memory, ForeignSignalPolicy::forward);

    const auto target = reinterpret_cast<std::uint64_t>(&debug_target);
    const auto before = read_u32(memory, reinterpret_cast<std::uint64_t>(&g_usr1)).value_or(0);
    REQUIRE(session.attach().has_value());
    REQUIRE(session.arm_software(0, target, true).has_value());

    const std::array<std::byte, 1> go {std::byte {1}};
    REQUIRE(slopkit::platform::write_bytes(child.pid(), reinterpret_cast<std::uint64_t>(&g_go), go).has_value());

    const auto stop = session.resume(0);
    REQUIRE(stop.has_value());
    CHECK(stop->reason == StopReason::breakpoint);
    CHECK(stop->trap_address == target);
    CHECK(read_u32(memory, reinterpret_cast<std::uint64_t>(&g_usr1)).value_or(0) > before);

    REQUIRE(session.detach().has_value());
}

TEST_CASE("debug session survives a target job-control pair", "[debug_session]")
{
    // SIGSTOP then SIGCONT, as the Wine server does to a thread; the forward
    // policy must not report either as a stop.
    ChildProcess child(
        []
        {
            while (g_go == 0)
            {
            }
            ::raise(SIGSTOP);
            ::raise(SIGCONT);
            for (;;)
            {
                debug_target();
            }
        });

    MemAccess    memory(child.pid());
    DebugSession session(child.pid(), memory, ForeignSignalPolicy::forward);

    const auto target = reinterpret_cast<std::uint64_t>(&debug_target);
    REQUIRE(session.attach().has_value());
    REQUIRE(session.arm_software(0, target, true).has_value());

    const std::array<std::byte, 1> go {std::byte {1}};
    REQUIRE(slopkit::platform::write_bytes(child.pid(), reinterpret_cast<std::uint64_t>(&g_go), go).has_value());

    const auto stop = session.resume(0);
    REQUIRE(stop.has_value());
    CHECK(stop->reason == StopReason::breakpoint);
    CHECK(stop->trap_address == target);
    CHECK(session.attached());

    REQUIRE(session.detach().has_value());
}

TEST_CASE("debug session reports its own interrupt", "[debug_session]")
{
    ChildProcess child(
        []
        {
            for (;;)
            {
                debug_target();
            }
        });

    MemAccess    memory(child.pid());
    DebugSession session(child.pid(), memory, ForeignSignalPolicy::forward);

    // The attaching thread must also issue the ptrace calls, so attach and
    // resume run on the same worker thread while the interrupt comes from here.
    std::promise<bool>                                        attached;
    bool                                                      detached = false;
    std::expected<slopkit::platform::StopStatus, AccessError> stop {std::unexpected(AccessError::internal)};
    std::thread                                               runner(
        [&session, &stop, &attached, &detached]
        {
            const auto leader = session.attach();
            attached.set_value(leader.has_value());
            if (!leader)
            {
                return;
            }
            stop     = session.resume(0);
            detached = session.detach().has_value();
        });
    REQUIRE(attached.get_future().get());

    const auto tick = read_u32(memory, reinterpret_cast<std::uint64_t>(&g_tick)).value_or(0);
    REQUIRE(await_tick(memory, tick));
    REQUIRE(session.interrupt(child.pid()).has_value());
    runner.join();

    REQUIRE(stop.has_value());
    CHECK(stop->reason == StopReason::interrupt);
    CHECK(detached);
}

TEST_CASE("debug session arms a hardware watch after its own interrupt", "[debug_session]")
{
    // The controller stops a running target with its own SIGSTOP to install a
    // data breakpoint. The stop resume() reports is the maintenance stop, and
    // the arm must land on the stopped tracee right after it.
    ChildProcess child(
        []
        {
            for (;;)
            {
                debug_target();
            }
        });

    MemAccess    memory(child.pid());
    DebugSession session(child.pid(), memory, ForeignSignalPolicy::suppress);

    const auto watched = reinterpret_cast<std::uint64_t>(&g_tick);

    std::promise<bool>                                        attached;
    std::expected<void, AccessError>                          armed {std::unexpected(AccessError::internal)};
    std::expected<slopkit::platform::StopStatus, AccessError> stop {std::unexpected(AccessError::internal)};
    bool                                                      detached = false;
    std::thread                                               runner(
        [&session, &stop, &attached, &armed, &detached, watched]
        {
            const auto leader = session.attach();
            attached.set_value(leader.has_value());
            if (!leader)
            {
                return;
            }
            stop     = session.resume(0);
            armed    = session.arm_hardware(0, 3, watched, 4, true);
            detached = session.detach().has_value();
        });
    REQUIRE(attached.get_future().get());

    const auto tick = read_u32(memory, reinterpret_cast<std::uint64_t>(&g_tick)).value_or(0);
    REQUIRE(await_tick(memory, tick));
    REQUIRE(session.interrupt(child.pid()).has_value());
    runner.join();

    REQUIRE(stop.has_value());
    CHECK(stop->reason == StopReason::interrupt);
    if (!armed)
    {
        INFO("arm error: " << static_cast<int>(armed.error()));
    }
    CHECK(armed.has_value());
    CHECK(detached);
}

TEST_CASE("debug session arms a hardware watch on a threaded target", "[debug_session]")
{
    ChildProcess child(
        []
        {
            ::clone(thread_loop,
                    g_thread_stack.data() + g_thread_stack.size(),
                    CLONE_THREAD | CLONE_VM | CLONE_SIGHAND | CLONE_FS | CLONE_FILES,
                    nullptr);
            for (;;)
            {
                debug_target();
            }
        });

    MemAccess    memory(child.pid());
    DebugSession session(child.pid(), memory, ForeignSignalPolicy::suppress);
    REQUIRE(await_threads(child.pid(), 2));

    const auto watched = reinterpret_cast<std::uint64_t>(&g_tick);

    std::promise<bool>                                        attached;
    std::expected<void, AccessError>                          armed {std::unexpected(AccessError::internal)};
    std::expected<slopkit::platform::StopStatus, AccessError> stop {std::unexpected(AccessError::internal)};
    bool                                                      detached = false;
    std::thread                                               runner(
        [&session, &stop, &attached, &armed, &detached, watched]
        {
            const auto leader = session.attach();
            attached.set_value(leader.has_value());
            if (!leader)
            {
                return;
            }
            stop     = session.resume(0);
            armed    = session.arm_hardware(0, 3, watched, 4, true);
            detached = session.detach().has_value();
        });
    REQUIRE(attached.get_future().get());

    const auto tick = read_u32(memory, reinterpret_cast<std::uint64_t>(&g_tick)).value_or(0);
    REQUIRE(await_tick(memory, tick));
    REQUIRE(session.interrupt(child.pid()).has_value());
    runner.join();

    REQUIRE(stop.has_value());
    CHECK(stop->reason == StopReason::interrupt);
    if (!armed)
    {
        INFO("arm error: " << static_cast<int>(armed.error()));
    }
    CHECK(armed.has_value());
    CHECK(detached);
}

TEST_CASE("debug session runs a remote syscall at its stops", "[debug_session]")
{
    ChildProcess child(
        []
        {
            for (;;)
            {
                debug_target();
            }
        });

    MemAccess    memory(child.pid());
    DebugSession session(child.pid(), memory, ForeignSignalPolicy::suppress);

    const auto leader = session.attach();
    REQUIRE(leader.has_value());

    const auto gadget = find_syscall_gadget(memory);
    REQUIRE(gadget != 0);

    const auto saved = session.registers(*leader);
    REQUIRE(saved.has_value());
    REQUIRE(session.set_register(*leader, "RIP", gadget).has_value());
    REQUIRE(session.set_register(*leader, "RAX", SYS_getpid).has_value());

    const auto entry = session.step_syscall(*leader);
    REQUIRE(entry.has_value());
    CHECK(entry->reason == StopReason::syscall);
    CHECK(entry->address == gadget + 2);
    REQUIRE(entry->syscall_entry.has_value());
    CHECK(*entry->syscall_entry);

    const auto exited = session.step_syscall(*leader);
    REQUIRE(exited.has_value());
    CHECK(exited->reason == StopReason::syscall);
    CHECK(exited->address == gadget + 2);
    REQUIRE(exited->syscall_entry.has_value());
    CHECK_FALSE(*exited->syscall_entry);

    const auto result = session.registers(*leader);
    REQUIRE(result.has_value());
    CHECK(result->rax == static_cast<std::uint64_t>(child.pid()));

    REQUIRE(session.set_register(*leader, "RIP", saved->rip).has_value());
    REQUIRE(session.set_register(*leader, "RAX", saved->rax).has_value());
    REQUIRE(session.detach().has_value());
}

TEST_CASE("debug session reports a killed target", "[debug_session]")
{
    ChildProcess child(
        []
        {
            for (;;)
            {
                debug_target();
            }
        });

    MemAccess    memory(child.pid());
    DebugSession session(child.pid(), memory, ForeignSignalPolicy::forward);

    std::promise<bool>                                        attached;
    std::expected<slopkit::platform::StopStatus, AccessError> stop {std::unexpected(AccessError::internal)};
    std::thread                                               runner(
        [&session, &stop, &attached]
        {
            const auto leader = session.attach();
            attached.set_value(leader.has_value());
            if (!leader)
            {
                return;
            }
            stop = session.resume(0);
        });
    REQUIRE(attached.get_future().get());

    const auto tick = read_u32(memory, reinterpret_cast<std::uint64_t>(&g_tick)).value_or(0);
    REQUIRE(await_tick(memory, tick));
    REQUIRE(::kill(static_cast<pid_t>(child.pid()), SIGKILL) == 0);
    runner.join();

    REQUIRE(stop.has_value());
    CHECK(stop->reason == StopReason::signalled);
    CHECK(stop->signal == SIGKILL);
}

TEST_CASE("debug session leaves an excluded thread running", "[debug_session]")
{
    ChildProcess child(
        []
        {
            ::clone(marked_loop,
                    g_thread_stack.data() + g_thread_stack.size(),
                    CLONE_THREAD | CLONE_VM | CLONE_SIGHAND | CLONE_FS | CLONE_FILES,
                    nullptr);
            for (;;)
            {
                debug_target();
            }
        });

    MemAccess    memory(child.pid());
    DebugSession session(child.pid(), memory, ForeignSignalPolicy::suppress);
    REQUIRE(await_threads(child.pid(), 2));

    std::uint32_t marked_tid = 0;
    for (int waited = 0; waited < 2000 && marked_tid == 0; waited += 5)
    {
        if (const auto value = read_u32(memory, reinterpret_cast<std::uint64_t>(&g_marked_tid)); value && *value != 0)
        {
            marked_tid = *value;
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    REQUIRE(marked_tid != 0);
    CHECK(marked_tid != child.pid());

    const std::array<ProcessId, 1> excluded {marked_tid};
    const auto                     leader = session.attach(std::span<const ProcessId>(excluded));
    REQUIRE(leader.has_value());
    CHECK(session.attached());

    const auto before = read_u32(memory, reinterpret_cast<std::uint64_t>(&g_marked)).value_or(0);
    REQUIRE(await_marked(memory, before));

    const auto frozen = read_u32(memory, reinterpret_cast<std::uint64_t>(&g_tick)).value_or(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(read_u32(memory, reinterpret_cast<std::uint64_t>(&g_tick)).value_or(~0u) == frozen);

    CHECK(session.detach().has_value());
    CHECK_FALSE(session.attached());
}
