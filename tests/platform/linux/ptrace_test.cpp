#include <catch2/catch.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <csignal>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#include "platform/linux/procfs.hpp"
#include "platform/linux/ptrace.hpp"
#include "process/types.hpp"

namespace
{
    using slopkit::platform::StopReason;
    using slopkit::process::AccessError;
    using slopkit::process::ProcessId;

    // A forked child stuck in pause(), so it stays alive and is always in a
    // known place while the test attaches to it. Forked children are traceable
    // even under Yama's ptrace_scope=1.
    class ChildProcess
    {
    public:
        ChildProcess()
        {
            const pid_t pid = ::fork();
            if (pid == 0)
            {
                // The test process may ignore some signals; the child must use
                // the default disposition so a delivered signal is observable.
                ::signal(SIGUSR1, SIG_DFL);
                ::signal(SIGTERM, SIG_DFL);
                for (;;)
                {
                    ::pause();
                }
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
                int status = 0;
                ::waitpid(static_cast<pid_t>(pid_), &status, 0);
            }
        }

        ChildProcess(const ChildProcess&)            = delete;
        ChildProcess& operator=(const ChildProcess&) = delete;

        [[nodiscard]] ProcessId pid() const noexcept
        {
            return pid_;
        }

        [[nodiscard]] bool valid() const noexcept
        {
            return pid_ != 0;
        }

    private:
        ProcessId pid_ {};
    };
}

TEST_CASE("ptrace seizes, stops and round-trips registers", "[ptrace]")
{
    ChildProcess child;
    REQUIRE(child.valid());

    const auto seized = slopkit::platform::seize(child.pid());
    REQUIRE(seized.has_value());
    CHECK(slopkit::platform::interrupt(child.pid()).has_value());

    const auto stop = slopkit::platform::wait(std::array {child.pid()});
    REQUIRE(stop.has_value());
    CHECK(stop->reason == StopReason::interrupt);
    CHECK(stop->tid == child.pid());

    const auto registers = slopkit::platform::get_registers(child.pid());
    REQUIRE(registers.has_value());
    CHECK(registers->rip != 0);

    CHECK(slopkit::platform::set_register(child.pid(), "rax", 0x1234'5678'9ABC'DEF0ULL).has_value());
    CHECK(slopkit::platform::set_register(child.pid(), "eflags", registers->rflags).has_value());
    const auto reread = slopkit::platform::get_registers(child.pid());
    REQUIRE(reread.has_value());
    CHECK(reread->rax == 0x1234'5678'9ABC'DEF0ULL);
    CHECK(reread->rip == registers->rip);

    // An unknown register name is refused instead of silently ignored.
    CHECK_FALSE(slopkit::platform::set_register(child.pid(), "not-a-register", 1).has_value());

    CHECK(slopkit::platform::detach(child.pid()).has_value());
}

TEST_CASE("ptrace round-trips the whole register file, not only named fields", "[ptrace]")
{
    ChildProcess child;
    REQUIRE(child.valid());

    REQUIRE(slopkit::platform::seize(child.pid()).has_value());
    REQUIRE(slopkit::platform::interrupt(child.pid()).has_value());
    REQUIRE(slopkit::platform::wait(std::array {child.pid()}).has_value());

    const auto raw = slopkit::platform::read_registers_raw(child.pid());
    REQUIRE(raw.has_value());
    REQUIRE(raw->size() == sizeof(user_regs_struct));

    CHECK(slopkit::platform::write_registers_raw(child.pid(), *raw).has_value());
    const auto again = slopkit::platform::read_registers_raw(child.pid());
    REQUIRE(again.has_value());
    CHECK(*again == *raw);

    CHECK(slopkit::platform::set_register(child.pid(), "ORIG_RAX", 0x7FFF'FFFF'FFFF'FFFFULL).has_value());
    const auto disarmed = slopkit::platform::read_registers_raw(child.pid());
    REQUIRE(disarmed.has_value());
    CHECK(*disarmed != *raw);

    CHECK(slopkit::platform::write_registers_raw(child.pid(), *raw).has_value());
    const auto restored = slopkit::platform::read_registers_raw(child.pid());
    REQUIRE(restored.has_value());
    CHECK(*restored == *raw);

    // A buffer that is not the whole file is refused instead of read past.
    CHECK_FALSE(slopkit::platform::write_registers_raw(child.pid(), std::span<const std::byte> {}).has_value());

    CHECK(slopkit::platform::detach(child.pid()).has_value());
}

TEST_CASE("ptrace writes and clears hardware debug registers", "[ptrace]")
{
    ChildProcess child;
    REQUIRE(child.valid());

    REQUIRE(slopkit::platform::seize(child.pid()).has_value());
    REQUIRE(slopkit::platform::interrupt(child.pid()).has_value());
    REQUIRE(slopkit::platform::wait(std::array {child.pid()}).has_value());

    CHECK(slopkit::platform::set_debug_register(child.pid(), 0, 0x4000).has_value());
    const auto slot0 = slopkit::platform::get_debug_register(child.pid(), 0);
    REQUIRE(slot0.has_value());
    CHECK(*slot0 == 0x4000);

    CHECK(slopkit::platform::set_debug_register(child.pid(), 7, 0x1).has_value());
    const auto control = slopkit::platform::get_debug_register(child.pid(), 7);
    REQUIRE(control.has_value());
    CHECK(*control == 0x1);

    CHECK(slopkit::platform::clear_debug_registers(child.pid()).has_value());
    REQUIRE(slopkit::platform::get_debug_register(child.pid(), 0).has_value());
    CHECK(*slopkit::platform::get_debug_register(child.pid(), 0) == 0);
    REQUIRE(slopkit::platform::get_debug_register(child.pid(), 7).has_value());
    CHECK(*slopkit::platform::get_debug_register(child.pid(), 7) == 0);

    CHECK_FALSE(slopkit::platform::set_debug_register(child.pid(), 8, 1).has_value());

    CHECK(slopkit::platform::detach(child.pid()).has_value());
}

TEST_CASE("ptrace reads and writes target words and bytes", "[ptrace]")
{
    ChildProcess child;
    REQUIRE(child.valid());

    REQUIRE(slopkit::platform::seize(child.pid()).has_value());
    REQUIRE(slopkit::platform::interrupt(child.pid()).has_value());
    REQUIRE(slopkit::platform::wait(std::array {child.pid()}).has_value());

    const auto registers = slopkit::platform::get_registers(child.pid());
    REQUIRE(registers.has_value());

    const auto word = slopkit::platform::peek_data(child.pid(), registers->rsp);
    REQUIRE(word.has_value());

    const auto bytes = slopkit::platform::read_bytes(child.pid(), registers->rsp, 8);
    REQUIRE(bytes.has_value());
    CHECK(bytes->size() == 8);
    CHECK(slopkit::platform::write_bytes(child.pid(), registers->rsp, *bytes).has_value());
    const auto reread = slopkit::platform::read_bytes(child.pid(), registers->rsp, 8);
    REQUIRE(reread.has_value());
    CHECK(*reread == *bytes);

    CHECK(slopkit::platform::poke_data(child.pid(), registers->rsp, *word).has_value());
    const auto word_again = slopkit::platform::peek_data(child.pid(), registers->rsp);
    REQUIRE(word_again.has_value());
    CHECK(*word_again == *word);

    CHECK_FALSE(slopkit::platform::peek_data(child.pid(), 0x1).has_value());

    CHECK(slopkit::platform::detach(child.pid()).has_value());
}

TEST_CASE("ptrace continues, interrupts and detaches a running target", "[ptrace]")
{
    ChildProcess child;
    REQUIRE(child.valid());

    REQUIRE(slopkit::platform::seize(child.pid()).has_value());
    REQUIRE(slopkit::platform::interrupt(child.pid()).has_value());
    REQUIRE(slopkit::platform::wait(std::array {child.pid()}).has_value());

    CHECK(slopkit::platform::cont(child.pid()).has_value());
    CHECK(slopkit::platform::interrupt(child.pid()).has_value());
    const auto stop = slopkit::platform::wait(std::array {child.pid()});
    REQUIRE(stop.has_value());
    CHECK(stop->reason == StopReason::interrupt);

    // Detaching resumes the target and clears the tracer pid.
    CHECK(slopkit::platform::detach(child.pid()).has_value());

    int status = 0;
    CHECK(::waitpid(static_cast<pid_t>(child.pid()), &status, WNOHANG) == 0);

    const auto live = slopkit::platform::read_status(child.pid());
    REQUIRE(live.has_value());
    CHECK(live->tracer_pid == 0);
}

TEST_CASE("ptrace tells a signal delivery stop from a signal death", "[ptrace]")
{
    ChildProcess child;
    REQUIRE(child.valid());

    REQUIRE(slopkit::platform::seize(child.pid()).has_value());
    REQUIRE(slopkit::platform::interrupt(child.pid()).has_value());
    REQUIRE(slopkit::platform::wait(std::array {child.pid()}).has_value());

    REQUIRE(slopkit::platform::cont(child.pid()).has_value());
    REQUIRE(::kill(static_cast<pid_t>(child.pid()), SIGUSR1) == 0);
    const auto delivery = slopkit::platform::wait(std::array {child.pid()});
    REQUIRE(delivery.has_value());
    CHECK(delivery->reason == StopReason::signal_stop);
    CHECK(delivery->signal == SIGUSR1);
    REQUIRE(slopkit::platform::read_status(child.pid()).has_value());

    REQUIRE(slopkit::platform::cont(child.pid(), SIGUSR1).has_value());
    const auto death = slopkit::platform::wait(std::array {child.pid()});
    REQUIRE(death.has_value());
    CHECK(death->reason == StopReason::signalled);
    CHECK(death->signal == SIGUSR1);
}

TEST_CASE("ptrace reports a missing thread", "[ptrace]")
{
    constexpr ProcessId missing = 0xF000'0000u;
    const auto          seized  = slopkit::platform::seize(missing);
    REQUIRE_FALSE(seized.has_value());
    CHECK(seized.error() == AccessError::not_found);

    const auto registers = slopkit::platform::get_registers(missing);
    REQUIRE_FALSE(registers.has_value());
    CHECK(registers.error() == AccessError::not_found);
}
