#include <catch2/catch.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <latch>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "platform/linux/procfs.hpp"
#include "platform/linux/wine.hpp"
#include "plugins/support/session.hpp"
#include "plugins/wine_proton/agent/slopkit_agent_protocol.h"
#include "plugins/wine_proton/agent_client.hpp"
#include "plugins/wine_proton/agent_regions.hpp"
#include "plugins/wine_proton/agent_service.hpp"
#include "plugins/wine_proton/wine_target.hpp"
#include "support/wine_helpers.hpp"

namespace
{
    using slopkit::plugins::support::AccessFault;
    using slopkit::plugins::support::Session;
    using slopkit::plugins::wine_proton::AgentClient;
    using slopkit::plugins::wine_proton::AgentService;
    using slopkit::plugins::wine_proton::derive_wine_target;
    using slopkit::plugins::wine_proton::linux_path_to_windows;
    using slopkit::process::ModuleInfo;

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

    // Starts `wine <arguments...>` with the ambient environment and waits,
    // bounded, for the probe's ready line, which also proves the target is up
    // before the injector is pointed at it. The returned pid is the direct
    // child, which is the process wine starts for the first argument.
    std::optional<ProbeLaunch> launch_probe(const std::filesystem::path&    wine,
                                            const std::vector<std::string>& arguments,
                                            std::chrono::milliseconds       timeout)
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
            std::vector<std::string> arguments_copy;
            arguments_copy.push_back(wine.string());
            arguments_copy.insert(arguments_copy.end(), arguments.begin(), arguments.end());
            std::vector<char*> argv;
            argv.reserve(arguments_copy.size() + 1);
            for (auto& entry : arguments_copy)
            {
                argv.push_back(entry.data());
            }
            argv.push_back(nullptr);
            ::execv(argv[0], argv.data());
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

    // The process wine handed a wineserver socket to: a process started from
    // another Wine process (`wine cmd /c <probe>`) is started by wineserver, so
    // it is a grandchild of this test and carries `WINESERVERSOCKET=<fd>` in
    // its own environment. It is found by name, and the handoff is required so
    // the loader process — whose command line names the probe too — is never
    // picked.
    std::optional<slopkit::process::ProcessId> find_handoff_target(std::string_view          probe_name,
                                                                   std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            for (const auto pid : slopkit::platform::list_pids())
            {
                const auto parts = slopkit::platform::read_cmdline_parts(pid);
                if (parts.empty() || parts.front().find(probe_name) == std::string::npos)
                {
                    continue;
                }
                const auto environment = slopkit::platform::read_environ(pid);
                const bool handed_off  = std::ranges::any_of(environment,
                                                             [](const std::string& entry)
                                                             {
                                                                return entry.starts_with("WINESERVERSOCKET=");
                                                             });
                if (handed_off)
                {
                    return pid;
                }
            }
            std::this_thread::sleep_for(50ms);
        }
        return std::nullopt;
    }
}

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
    const auto      probe = wine_probe();
    std::error_code error;
    if (wine.empty() || !std::filesystem::exists(probe, error))
    {
        WARN("wine or the 64-bit test probe is unavailable; the end-to-end agent test is skipped");
        return;
    }

    ::setenv("SLOPKIT_AGENT_DIR", SLOPKIT_AGENT_BINARY_DIR, 1);

    auto launched = launch_probe(wine, {probe.string()}, 120s);
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

    // The bytes are the host's business now: the agent only allocated the
    // block, and a plain process_vm transfer moves the pattern in and out.
    slopkit::plugins::support::Session session(launched->guard.pid, slopkit::platform::ForeignSignalPolicy::forward);

    std::vector<std::byte> pattern(16);
    for (std::size_t index = 0; index < pattern.size(); ++index)
    {
        pattern[index] = static_cast<std::byte>(0x40 + index);
    }
    const auto written = session.mem.write(*allocated, pattern);
    REQUIRE(written.has_value());
    CHECK(written->bytes == pattern.size());
    CHECK(written->primitive != slopkit::platform::MemoryPrimitive::none);

    std::vector<std::byte> readback(pattern.size());
    const auto             read = session.mem.read(*allocated, readback);
    REQUIRE(read.has_value());
    CHECK(read->bytes == pattern.size());
    CHECK(readback == pattern);

    // Several host threads read the same block at once: the host primitives are
    // independent per call, so no slot ring stands between them.
    {
        std::atomic<bool>        failed {false};
        std::vector<std::thread> readers;
        for (int index = 0; index < 4; ++index)
        {
            readers.emplace_back(
                [&, index]
                {
                    std::vector<std::byte> chunk(4);
                    const auto chunk_read = session.mem.read(*allocated + static_cast<std::uint64_t>(index) * 4, chunk);
                    if (!chunk_read.has_value() || chunk_read->bytes != chunk.size())
                    {
                        failed = true;
                    }
                });
        }
        for (auto& reader : readers)
        {
            reader.join();
        }
        CHECK_FALSE(failed);
    }

    bool          saw_block = false;
    bool          saw_agent = false;
    std::uint64_t cursor    = 0;
    for (int guard = 0; guard < 100000 && !(saw_block && saw_agent); ++guard)
    {
        const auto batch = agent->enumerate(cursor, 0, 5s);
        REQUIRE(batch.has_value());
        for (const auto& entry : batch->regions)
        {
            if (*allocated >= entry.base && *allocated < entry.base + entry.size)
            {
                saw_block = true;
            }
            if (agent->agent_base() >= entry.base && agent->agent_base() < entry.base + entry.size)
            {
                saw_agent = true;
            }
        }
        cursor = batch->next_cursor;
        if (cursor == 0)
        {
            break;
        }
    }
    CHECK(saw_block);
    CHECK(saw_agent);

    // The module walk reads the target's own loader list (InLoadOrderModuleList),
    // so it names the probe itself, kernel32 and ntdll, every base is a real file
    // mapping in /proc, and no host .so mapping leaks into the list.
    const auto maps = slopkit::platform::read_maps(launched->guard.pid);

    std::vector<slopkit::plugins::wine_proton::AgentModuleEntry> modules;
    std::uint64_t                                                module_cursor = 0;
    for (int guard = 0; guard < 100000; ++guard)
    {
        const auto batch = agent->modules(module_cursor, 0, 5s);
        REQUIRE(batch.has_value());
        modules.insert(modules.end(), batch->modules.begin(), batch->modules.end());
        module_cursor = batch->next_index;
        if (module_cursor == 0)
        {
            break;
        }
    }
    REQUIRE_FALSE(modules.empty());

    const auto same_name = [](std::string_view left, std::string_view right)
    {
        if (left.size() != right.size())
        {
            return false;
        }
        for (std::size_t index = 0; index < left.size(); ++index)
        {
            if (std::tolower(static_cast<unsigned char>(left[index]))
                != std::tolower(static_cast<unsigned char>(right[index])))
            {
                return false;
            }
        }
        return true;
    };

    bool saw_probe  = false;
    bool saw_kernel = false;
    bool saw_ntdll  = false;
    for (const auto& module : modules)
    {
        INFO("module " << module.name);
        if (module.main)
        {
            saw_probe = same_name(module.name, "slopkit-probe-x86_64.exe");
        }
        saw_kernel = saw_kernel || same_name(module.name, "kernel32.dll");
        saw_ntdll  = saw_ntdll || same_name(module.name, "ntdll.dll");

        CHECK(module.name.find(".so") == std::string::npos);

        const auto covering = std::ranges::find_if(maps,
                                                   [&](const slopkit::platform::MappedRegion& map)
                                                   {
                                                       return map.start <= module.base && module.base < map.end;
                                                   });
        CHECK(covering != maps.end());
    }
    CHECK(saw_probe);
    CHECK(saw_kernel);
    CHECK(saw_ntdll);

    const auto block = agent->query(*allocated, 5s);
    REQUIRE(block.has_value());
    CHECK(block->state == SLOPKIT_AGENT_MEM_COMMIT);
    CHECK(slopkit::plugins::wine_proton::region_protection_readable(block->protection));

    const auto unmapped = agent->query(0x10, 5s);
    REQUIRE(unmapped.has_value());
    CHECK(unmapped->state != SLOPKIT_AGENT_MEM_COMMIT);

    // The debug-attach exemption hook must recognise the agent's threads in a
    // real wine target, so a debug attach can leave them running while the rest
    // of the target is stopped.
    {
        slopkit::plugins::support::Session session(launched->guard.pid,
                                                   slopkit::platform::ForeignSignalPolicy::forward);
        const auto                         tids = slopkit::plugins::wine_proton::AgentService::agent_threads(session);
        CHECK_FALSE(tids.empty());
        for (const auto tid : tids)
        {
            CHECK(tid != static_cast<std::uint32_t>(launched->guard.pid));
        }
    }

    REQUIRE(agent->free(*near, 5s).has_value());
    CHECK(agent->owns(*allocated));
    REQUIRE(agent->free(*allocated, 5s).has_value());
    CHECK_FALSE(agent->owns(*allocated));

    agent->shutdown(5s);
}

// Proton starts a game from another Wine process, so the game inherits
// `WINESERVERSOCKET=<fd>`: the wineserver socket its parent already had open.
// That descriptor only exists in the parent's table, so running the injector
// with the entry in its environment made Wine's loader abort with "Bad server
// socket <fd> : Bad file descriptor" before it ever loaded the agent. `cmd /c`
// starts the probe the same way wine starts a game, so this is the local
// reproduction of the Proton-only failure; the target is the process wine
// handed the socket to, which is the direct child here.
//
// Run it explicitly with:
//   SLOPKIT_WINE_E2E=1 ctest --test-dir build -R "inherited a wineserver socket" --timeout 600 --output-on-failure
TEST_CASE("a target that inherited a wineserver socket is still injectable", "[wine-proton][agent][.]")
{
    const char* enabled = std::getenv("SLOPKIT_WINE_E2E");
    if (enabled == nullptr || std::string(enabled) != "1")
    {
        WARN("set SLOPKIT_WINE_E2E=1 to run the wine end-to-end agent test");
        return;
    }

    const auto      wine  = find_wine();
    const auto      probe = wine_probe();
    std::error_code error;
    if (wine.empty() || !std::filesystem::exists(probe, error))
    {
        WARN("wine or the 64-bit test probe is unavailable; the handoff test is skipped");
        return;
    }

    ::setenv("SLOPKIT_AGENT_DIR", SLOPKIT_AGENT_BINARY_DIR, 1);

    auto launched = launch_probe(wine, {"cmd", "/c", linux_path_to_windows(probe.string())}, 120s);
    if (!launched.has_value())
    {
        WARN("the probe did not start through cmd; this wine cannot reproduce the handoff");
        return;
    }

    const auto probe_pid = find_handoff_target(probe.filename().string(), 30s);
    if (!probe_pid.has_value())
    {
        // A wine that does not hand its socket down this way cannot reproduce
        // the target's situation, so there is nothing to check here.
        WARN("this wine did not hand a wineserver socket to the probe");
        return;
    }
    // The probe is wine's own child, so it is not the one launch_probe guards.
    ChildGuard probe_guard(*probe_pid);

    const auto target = derive_wine_target(*probe_pid);
    REQUIRE(target.has_value());
    // The handoff is process-local: the environment the injector runs with must
    // not carry it, or wine's loader refuses to start.
    CHECK(std::ranges::none_of(target->environment,
                               [](const std::string& entry)
                               {
                                   return entry.starts_with("WINESERVERSOCKET=");
                               }));

    auto agent = AgentClient::launch(*target, 120s);
    if (!agent.has_value())
    {
        FAIL("the agent could not be launched into a target that inherited a wineserver socket: " << agent.error());
    }

    const auto allocated = agent->allocate(0x1000, 0, 5s);
    REQUIRE(allocated.has_value());
    CHECK(agent->owns(*allocated));
    agent->shutdown(5s);
}

// The scanner's first module fetch is the same call that stages, injects and
// handshakes the resident agent, so a second fetch that lands while that launch
// is still running must wait for it instead of reporting "the resident agent is
// not available"; otherwise module names and RVAs stay absent until the next
// attach. Both concurrent fetches must therefore come back with the loader walk.
//
// Run it explicitly with:
//   SLOPKIT_WINE_E2E=1 ctest --test-dir build -R "meets a bring-up in flight" --timeout 600 --output-on-failure
TEST_CASE("a module fetch that meets a bring-up in flight waits for it", "[wine-proton][agent][.]")
{
    const char* enabled = std::getenv("SLOPKIT_WINE_E2E");
    if (enabled == nullptr || std::string(enabled) != "1")
    {
        WARN("set SLOPKIT_WINE_E2E=1 to run the wine end-to-end agent test");
        return;
    }

    const auto      wine  = find_wine();
    const auto      probe = wine_probe();
    std::error_code error;
    if (wine.empty() || !std::filesystem::exists(probe, error))
    {
        WARN("wine or the 64-bit test probe is unavailable; the bring-up wait test is skipped");
        return;
    }

    ::setenv("SLOPKIT_AGENT_DIR", SLOPKIT_AGENT_BINARY_DIR, 1);

    auto launched = launch_probe(wine, {probe.string()}, 120s);
    if (!launched.has_value())
    {
        FAIL("the wine probe did not start; is a wine prefix available?");
    }

    constexpr std::size_t kCallers = 2;

    std::vector<std::unique_ptr<Session>> sessions;
    sessions.reserve(kCallers);
    for (std::size_t index = 0; index < kCallers; ++index)
    {
        sessions.push_back(
            std::make_unique<Session>(launched->guard.pid, slopkit::platform::ForeignSignalPolicy::forward));
    }

    std::vector<std::expected<std::vector<ModuleInfo>, AccessFault>> results(kCallers);
    std::latch                                                       ready(kCallers);
    std::latch                                                       go(1);

    std::vector<std::thread> callers;
    callers.reserve(kCallers);
    for (std::size_t index = 0; index < kCallers; ++index)
    {
        callers.emplace_back(
            [&, index]
            {
                ready.count_down();
                go.wait();
                results[index] = AgentService::modules(*sessions[index]);
            });
    }

    // Both fetch at once. Exactly one of them performs the bring-up and the
    // other must have observed that launch in flight: the stage and inject step
    // outlasts the thread start, so it cannot have missed it.
    ready.wait();
    go.count_down();
    for (auto& caller : callers)
    {
        caller.join();
    }

    for (std::size_t index = 0; index < kCallers; ++index)
    {
        INFO("caller " << index);
        REQUIRE(results[index].has_value());
        CHECK_FALSE(results[index]->empty());
    }

    AgentService::close(launched->guard.pid);
}
