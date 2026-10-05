#include <catch2/catch.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <future>
#include <optional>
#include <thread>

#include <sched.h>
#include <signal.h>
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

    // Shared with the forked child: it runs debug_target in a loop so the parent
    // always has a stable code address to break on, and counts the SIGUSR1
    // deliveries its own handler sees.
    volatile std::uint32_t g_tick = 0;
    volatile std::uint32_t g_usr1 = 0;
    volatile std::uint32_t g_go   = 0;

    extern "C" void usr1_handler(int)
    {
        g_usr1 = g_usr1 + 1;
    }

    __attribute__((noinline)) void debug_target()
    {
        g_tick = g_tick + 1;
    }

    // A forked child running the given body, so each test controls what the
    // target does without a second binary.
    class ChildProcess
    {
    public:
        template<class Body>
        explicit ChildProcess(Body body)
        {
            const pid_t pid = ::fork();
            if (pid == 0)
            {
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

    // Waits until the child has advanced past `above`, proving it is running.
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

    // Waits until the target reports `want` threads in procfs, so that a session
    // attaching right after sees them all.
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
} // namespace

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

    const auto leader = session.attach();
    REQUIRE(leader.has_value());
    CHECK(*leader == child.pid());
    CHECK(session.attached());

    // Attaching twice is idempotent.
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

    // Run into the software trap.
    REQUIRE(session.arm_software(0, target, true).has_value());
    CHECK(read_byte(memory, target) == std::byte {0xCC});

    const auto hit = session.resume(0);
    REQUIRE(hit.has_value());
    CHECK(hit->reason == StopReason::breakpoint);
    CHECK(hit->trap_address == target);
    CHECK(hit->address == target + 1);
    CHECK_FALSE(hit->debug_slot.has_value());

    // Step over the trap: the original byte runs and the trap is re-armed.
    const auto stepped = session.resume(target);
    REQUIRE(stepped.has_value());
    CHECK(stepped->reason == StopReason::single_step);
    CHECK(stepped->address != target);
    CHECK(read_byte(memory, target) == std::byte {0xCC});

    // A plain single step progresses one instruction.
    const auto single = session.step(stepped->tid);
    REQUIRE(single.has_value());
    CHECK(single->reason == StopReason::single_step);
    CHECK(single->address != stepped->address);

    const auto frames = session.backtrace(single->tid);
    REQUIRE(frames.has_value());
    REQUIRE_FALSE(frames->empty());
    CHECK(frames->front().pc == single->address);

    // Running on hits the re-armed trap again, then leave and remove it.
    const auto again = session.resume(0);
    REQUIRE(again.has_value());
    CHECK(again->reason == StopReason::breakpoint);
    CHECK(again->trap_address == target);

    const auto cleared = session.resume(target);
    REQUIRE(cleared.has_value());
    CHECK(cleared->reason == StopReason::single_step);
    REQUIRE(session.arm_software(0, target, false).has_value());
    CHECK(read_byte(memory, target) == *before);

    // A hardware execute breakpoint fires and names its DR slot.
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
    CHECK(session.detach().has_value()); // idempotent
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

    // Release the child now that it is stopped and the trap is armed.
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
    INFO("arm error: " << static_cast<int>(armed.error()));
    CHECK(armed.has_value());
    CHECK(detached);
}

TEST_CASE("debug session arms a hardware watch on a threaded target", "[debug_session]")
{
    // A threaded target: the maintenance SIGSTOP group-stops every thread, but
    // only the first stop is reaped before the arm runs.
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
    // Both threads have to exist before attaching, or the session seizes one.
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
    INFO("arm error: " << static_cast<int>(armed.error()));
    CHECK(armed.has_value());
    CHECK(detached);
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

    // Attach and resume on one thread; the fatal signal is delivered from here.
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
