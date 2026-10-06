#include <catch2/catch.hpp>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <string>
#include <thread>

#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "platform/linux/memory.hpp"
#include "platform/linux/process_control.hpp"
#include "process/types.hpp"

namespace
{
    using slopkit::platform::MemoryError;
    using slopkit::process::ProcessId;

    // A forked child stuck in pause(), so it stays alive in a known state
    // while the test signals it. Forked children are signalable even under
    // Yama's ptrace_scope=1.
    class ChildProcess
    {
    public:
        ChildProcess()
        {
            const pid_t pid = ::fork();
            if (pid == 0)
            {
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
                // SIGKILL terminates a stopped process as well.
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

    // The state letter from /proc/<pid>/stat, parsed after the parenthesised
    // command name so a name containing spaces cannot confuse it.
    char process_state(ProcessId pid)
    {
        std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
        std::string   line;
        if (!std::getline(stat, line))
        {
            return '\0';
        }
        const auto close = line.rfind(')');
        if (close == std::string::npos || close + 2 >= line.size())
        {
            return '\0';
        }
        return line[close + 2];
    }

    // SIGSTOP/SIGCONT delivery is asynchronous, so the state is polled with a
    // bounded wait instead of asserted immediately.
    bool wait_for_state(ProcessId pid, std::initializer_list<char> states, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;)
        {
            const char state = process_state(pid);
            for (const char candidate : states)
            {
                if (state == candidate)
                {
                    return true;
                }
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
} // namespace

TEST_CASE("process control suspends and resumes a running target", "[process_control]")
{
    ChildProcess child;
    REQUIRE(child.valid());

    // The child sleeps in pause(), i.e. it is running, not stopped.
    REQUIRE(wait_for_state(child.pid(), {'S', 'R'}, std::chrono::seconds(2)));

    REQUIRE(slopkit::platform::suspend_process(child.pid()).has_value());
    CHECK(wait_for_state(child.pid(), {'T'}, std::chrono::seconds(2)));

    REQUIRE(slopkit::platform::resume_process(child.pid()).has_value());
    CHECK(wait_for_state(child.pid(), {'S', 'R'}, std::chrono::seconds(2)));
}

TEST_CASE("process control tolerates repeated signals", "[process_control]")
{
    ChildProcess child;
    REQUIRE(child.valid());
    REQUIRE(wait_for_state(child.pid(), {'S', 'R'}, std::chrono::seconds(2)));

    // Stopping an already-stopped process and continuing an already-running
    // process are both harmless.
    CHECK(slopkit::platform::suspend_process(child.pid()).has_value());
    REQUIRE(wait_for_state(child.pid(), {'T'}, std::chrono::seconds(2)));
    CHECK(slopkit::platform::suspend_process(child.pid()).has_value());
    CHECK(wait_for_state(child.pid(), {'T'}, std::chrono::milliseconds(50)));

    CHECK(slopkit::platform::resume_process(child.pid()).has_value());
    REQUIRE(wait_for_state(child.pid(), {'S', 'R'}, std::chrono::seconds(2)));
    CHECK(slopkit::platform::resume_process(child.pid()).has_value());
    CHECK(wait_for_state(child.pid(), {'S', 'R'}, std::chrono::seconds(2)));
}

TEST_CASE("process control reports a missing target", "[process_control]")
{
    constexpr ProcessId missing = 0xF000'0000u;

    const auto suspended = slopkit::platform::suspend_process(missing);
    REQUIRE_FALSE(suspended.has_value());
    CHECK(suspended.error() == MemoryError::process_not_found);

    const auto resumed = slopkit::platform::resume_process(missing);
    REQUIRE_FALSE(resumed.has_value());
    CHECK(resumed.error() == MemoryError::process_not_found);
}
