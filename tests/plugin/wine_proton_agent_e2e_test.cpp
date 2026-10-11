#include <catch2/catch.hpp>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "plugins/wine_proton/agent/slopkit_agent_protocol.h"
#include "plugins/wine_proton/agent_client.hpp"
#include "plugins/wine_proton/wine_target.hpp"

namespace
{
    using slopkit::plugins::wine_proton::AgentClient;
    using slopkit::plugins::wine_proton::derive_wine_target;

    using namespace std::chrono_literals;

    // Kills and reaps the wine probe when the test leaves the scope.
    struct ChildGuard
    {
        pid_t pid {-1};

        ChildGuard() = default;

        explicit ChildGuard(pid_t child) : pid(child) {}

        ChildGuard(const ChildGuard&)            = delete;
        ChildGuard& operator=(const ChildGuard&) = delete;

        ChildGuard(ChildGuard&& other) noexcept : pid(std::exchange(other.pid, -1)) {}

        ChildGuard& operator=(ChildGuard&& other) noexcept
        {
            if (this != &other)
            {
                this->~ChildGuard();
                pid = std::exchange(other.pid, -1);
            }
            return *this;
        }

        ~ChildGuard()
        {
            if (pid > 0)
            {
                ::kill(pid, SIGKILL);
                ::waitpid(pid, nullptr, 0);
            }
        }
    };

    struct ProbeLaunch
    {
        ChildGuard  guard;
        std::string output;
    };

    std::filesystem::path find_wine()
    {
        for (const auto* candidate : {"/usr/bin/wine64", "/usr/bin/wine"})
        {
            std::error_code error;
            if (std::filesystem::exists(candidate, error))
            {
                return candidate;
            }
        }
        return {};
    }

    // Starts `wine <probe>` with the ambient environment and waits, bounded, for
    // the probe's ready line, which also proves the target is up before the
    // injector is pointed at it.
    std::optional<ProbeLaunch> launch_probe(const std::filesystem::path& wine,
                                            const std::filesystem::path& probe,
                                            std::chrono::milliseconds    timeout)
    {
        int pipes[2] = {-1, -1};
        if (::pipe2(pipes, O_CLOEXEC) != 0)
        {
            return std::nullopt;
        }

        const pid_t child = ::fork();
        if (child < 0)
        {
            ::close(pipes[0]);
            ::close(pipes[1]);
            return std::nullopt;
        }
        if (child == 0)
        {
            ::dup2(pipes[1], STDOUT_FILENO);
            ::dup2(pipes[1], STDERR_FILENO);
            ::close(pipes[0]);
            ::close(pipes[1]);
            const std::string program = wine.string();
            const std::string target  = probe.string();
            ::execl(program.c_str(), program.c_str(), target.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }
        ::close(pipes[1]);

        std::string output;
        bool        ready    = false;
        const auto  deadline = std::chrono::steady_clock::now() + timeout;
        while (!ready && std::chrono::steady_clock::now() < deadline)
        {
            pollfd    descriptor {pipes[0], POLLIN, 0};
            const int wait = ::poll(&descriptor, 1, 200);
            if (wait < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                break;
            }
            if (wait == 0)
            {
                continue;
            }
            char          buffer[512];
            const ssize_t count = ::read(pipes[0], buffer, sizeof(buffer));
            if (count <= 0)
            {
                break;
            }
            output.append(buffer, static_cast<std::size_t>(count));
            ready = output.find("slopkit-probe") != std::string::npos;
        }
        ::close(pipes[0]);

        if (!ready)
        {
            ::kill(child, SIGKILL);
            ::waitpid(child, nullptr, 0);
            return std::nullopt;
        }
        return ProbeLaunch {ChildGuard {child}, std::move(output)};
    }
} // namespace

// The only test that proves the whole chain against a real Wine process: the
// injector runs under the target's own wine, the DLL allocates through Wine's
// VirtualAlloc, and every operation crosses the shared region. It is hidden
// (tagged "[.]") and gated behind an environment variable because it needs
// mingw-w64, a working wine prefix and a real game is the manual recipe's job.
//
// Run it explicitly with:
//   SLOPKIT_WINE_E2E=1 ctest --test-dir build -R "resident agent allocates" --timeout 600 --output-on-failure
TEST_CASE("the resident agent allocates inside a real wine target", "[wine-proton][agent][.]")
{
    const char* enabled = std::getenv("SLOPKIT_WINE_E2E");
    if (enabled == nullptr || std::string(enabled) != "1")
    {
        WARN("set SLOPKIT_WINE_E2E=1 to run the wine end-to-end agent test");
        return;
    }

    const auto      wine  = find_wine();
    const auto      probe = std::filesystem::path(SLOPKIT_AGENT_PROBE_DIR) / "slopkit-probe-x86_64.exe";
    std::error_code error;
    if (wine.empty() || !std::filesystem::exists(probe, error))
    {
        WARN("wine or the 64-bit test probe is unavailable; the end-to-end agent test is skipped");
        return;
    }

    // The plugin normally resolves this through dladdr; the test points it at
    // the build's plugin directory directly.
    ::setenv("SLOPKIT_AGENT_DIR", SLOPKIT_AGENT_BINARY_DIR, 1);

    auto launched = launch_probe(wine, probe, 120s);
    if (!launched.has_value())
    {
        FAIL("the wine probe did not start; is a wine prefix available?");
    }

    const auto target = derive_wine_target(launched->guard.pid);
    REQUIRE(target.has_value());
    CHECK(target->pointer_size == 8);
    CHECK(target->image_path.starts_with("Z:"));
    CHECK(target->wine_binary.string().find("wine") != std::string::npos);

    auto agent = AgentClient::launch(*target, 120s);
    if (!agent.has_value())
    {
        FAIL("the agent could not be launched: " << agent.error());
    }

    CHECK(agent->windows_pid() != 0);
    CHECK(agent->granularity() == 0x10000u);

    const auto allocated = agent->allocate(0x1000, 0, 5s);
    REQUIRE(allocated.has_value());
    CHECK(*allocated != 0);
    CHECK(agent->owns(*allocated));

    // Wine's own manager sees the mapping, which is the whole point of the
    // resident agent: a raw mmap would report MEM_FREE here.
    const auto region = agent->query(*allocated, 5s);
    REQUIRE(region.has_value());
    CHECK(region->state == SLOPKIT_AGENT_MEM_COMMIT);
    CHECK(region->protection == SLOPKIT_AGENT_PAGE_EXECUTE_READWRITE);

    const auto near = agent->allocate_near(0x1000, *allocated, 5s);
    REQUIRE(near.has_value());
    CHECK(*near != 0);
    const auto distance = *near > *allocated ? *near - *allocated : *allocated - *near;
    CHECK(distance <= 0x80000000ull);

    const auto previous = agent->protect(*allocated, 0x1000, SLOPKIT_AGENT_PAGE_READONLY, 5s);
    REQUIRE(previous.has_value());
    CHECK(*previous == SLOPKIT_AGENT_PAGE_EXECUTE_READWRITE);
    REQUIRE(agent->protect(*allocated, 0x1000, SLOPKIT_AGENT_PAGE_EXECUTE_READWRITE, 5s).has_value());

    REQUIRE(agent->free(*near, 5s).has_value());
    CHECK(agent->owns(*allocated));
    REQUIRE(agent->free(*allocated, 5s).has_value());
    CHECK_FALSE(agent->owns(*allocated));

    // SHUTDOWN frees what is left and stops the service thread; the destructor
    // is idempotent, so the client can be left to clean up on its own.
    agent->shutdown(5s);
}
