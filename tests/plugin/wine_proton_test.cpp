#include <catch2/catch.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "platform/linux/procfs.hpp"
#include "platform/linux/wine.hpp"
#include "plugin/plugin_host.hpp"
#include "plugins/support/session.hpp"
#include "plugins/wine_proton/agent_client.hpp"
#include "plugins/wine_proton/agent_service.hpp"
#include "plugins/wine_proton/wine_target.hpp"
#include "process/plugin_access.hpp"
#include "process/types.hpp"

namespace
{
    using slopkit::process::AccessError;
    using slopkit::process::AccessMethod;
    using slopkit::process::ProcessId;

    constexpr std::size_t kPageSize = 4096;

    // Kills and reaps the forked child when the test leaves the scope.
    struct ChildGuard
    {
        pid_t pid {-1};

        ChildGuard() = default;

        explicit ChildGuard(pid_t child) : pid(child) {}

        ChildGuard(const ChildGuard&)            = delete;
        ChildGuard& operator=(const ChildGuard&) = delete;

        ~ChildGuard()
        {
            if (pid > 0)
            {
                ::kill(pid, SIGKILL);
                ::waitpid(pid, nullptr, 0);
            }
        }
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

    // SIGSTOP/SIGCONT delivery is asynchronous, so the stopped/running state is
    // polled with a bounded wait instead of asserted immediately.
    bool wait_for_stopped(ProcessId pid, bool stopped, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;)
        {
            const char state = process_state(pid);
            if (state != '\0' && (state == 'T') == stopped)
            {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    // Writes a single value to a /proc control file, which is how a user
    // namespace gets its single identity mapping.
    bool write_control_file(const std::string& path, const std::string& value)
    {
        std::ofstream file(path, std::ios::trunc);
        if (file)
        {
            file << value;
        }
        return file.good();
    }

    // Runs a child in its own mount namespace in which `hide` is an empty tmpfs,
    // so the host still sees that directory but the child cannot: the shape a
    // pressure-vessel container gives a Proton game. Returns the child, or -1
    // when the kernel refuses the namespaces.
    pid_t spawn_isolated_child(const std::filesystem::path& hide)
    {
        int ready[2] = {-1, -1};
        if (::pipe(ready) != 0)
        {
            return -1;
        }
        const pid_t child = ::fork();
        if (child < 0)
        {
            ::close(ready[0]);
            ::close(ready[1]);
            return -1;
        }
        if (child == 0)
        {
            ::close(ready[0]);
            if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0)
            {
                ::_exit(70);
            }
            // Becoming the mapped user of the new namespace is what makes the
            // mount below privileged enough to succeed.
            const bool mapped = write_control_file("/proc/self/setgroups", "deny")
                             && write_control_file("/proc/self/uid_map", std::format("0 {} 1\n", ::getuid()))
                             && write_control_file("/proc/self/gid_map", std::format("0 {} 1\n", ::getgid()));
            if (!mapped)
            {
                ::_exit(71);
            }
            (void)::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr);
            if (::mount("tmpfs", hide.c_str(), "tmpfs", 0, nullptr) != 0)
            {
                ::_exit(72);
            }
            if (::write(ready[1], "1", 1) != 1)
            {
                ::_exit(73);
            }
            ::close(ready[1]);
            for (;;)
            {
                ::pause();
            }
        }
        ::close(ready[1]);

        char       byte {'\0'};
        ssize_t    got      = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline && got != 1)
        {
            ::pollfd descriptor {ready[0], POLLIN, 0};
            if (::poll(&descriptor, 1, 50) > 0)
            {
                got = ::read(ready[0], &byte, 1);
                break;
            }
        }
        ::close(ready[0]);
        if (got != 1)
        {
            ::kill(child, SIGKILL);
            ::waitpid(child, nullptr, 0);
            return -1;
        }
        return child;
    }
} // namespace

TEST_CASE("wine-proton is discovered with the expected metadata", "[wine_proton]")
{
    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    const auto* plugin = host.find("wine-proton");
    REQUIRE(plugin != nullptr);
    CHECK(plugin->precedence() == 10);
    CHECK(slopkit::process::has_flag(plugin->access_methods(), AccessMethod::process_vm));
    CHECK(slopkit::process::has_flag(plugin->access_methods(), AccessMethod::procfs_mem));
}

TEST_CASE("wine-proton lists only processes it claims", "[wine_proton]")
{
    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("wine-proton");
    REQUIRE(plugin != nullptr);

    const auto processes = plugin->list_processes();
    REQUIRE(processes.has_value());
    for (const auto& process : *processes)
    {
        INFO("pid " << process.pid);
        CHECK(slopkit::platform::classify_wine_process(process.pid).claimed());
    }
}

TEST_CASE("wine-proton lists modules and threads of a session", "[wine_proton]")
{
    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});
    slopkit::process::PluginAccess access(host);

    const auto self    = static_cast<ProcessId>(::getpid());
    auto       session = access.attach(self, "wine-proton");
    REQUIRE(session.has_value());

    // Module enumeration is agent-only, so a native target reports the recorded
    // reason, never a silent host-derived list.
    const auto modules = session->modules();
    REQUIRE_FALSE(modules.has_value());
    CHECK(modules.error() == AccessError::io_error);

    const auto threads = session->threads();
    REQUIRE(threads.has_value());
    CHECK_FALSE(threads->empty());

    // The region list falls back to the host's /proc/maps view when the agent
    // cannot serve, so introspection survives a target the agent cannot reach.
    const auto regions = session->regions();
    REQUIRE(regions.has_value());
    CHECK_FALSE(regions->empty());
}

TEST_CASE("wine-proton falls back to the host path when the agent cannot serve", "[wine_proton]")
{
    // A real, readable page, so the host path can succeed where the agent
    // cannot even start.
    auto* page =
        static_cast<std::byte*>(::mmap(nullptr, kPageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(page != MAP_FAILED);
    const auto address = reinterpret_cast<std::uint64_t>(page);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});
    slopkit::process::PluginAccess access(host);

    auto session = access.attach(static_cast<ProcessId>(::getpid()), "wine-proton");
    REQUIRE(session.has_value());

    // A native target cannot run the resident agent, so reads and writes answer
    // from the host process_vm path instead of failing the session.
    const auto data = session->read(address, 64);
    REQUIRE(data.has_value());
    CHECK(data->size() == 64);

    const std::array<std::byte, 8> pattern {std::byte {0xde},
                                            std::byte {0xad},
                                            std::byte {0xbe},
                                            std::byte {0xef},
                                            std::byte {0x01},
                                            std::byte {0x02},
                                            std::byte {0x03},
                                            std::byte {0x04}};
    const auto                     written = session->write(address, pattern);
    REQUIRE(written.has_value());
    CHECK(*written == pattern.size());
    CHECK(std::equal(pattern.begin(), pattern.end(), page));

    ::munmap(page, kPageSize);
}

TEST_CASE("wine-proton serves a stopped target from the host path", "[wine_proton]")
{
    // A child that is not Wine, so the agent can never serve it; once the debug
    // session stops it, the host path must answer because the agent's service
    // thread cannot run.
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    ChildGuard guard(child);
    if (child == 0)
    {
        for (;;)
        {
            ::pause();
        }
    }

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("wine-proton");
    REQUIRE(plugin != nullptr);

    auto session = plugin->open_session(static_cast<ProcessId>(child));
    REQUIRE(session.has_value());
    REQUIRE(session->supports_debug());

    auto* vtable = session->vtable();
    REQUIRE(vtable != nullptr);
    void* handle = session->handle();

    std::uint32_t leader = 0;
    REQUIRE(vtable->debug_attach(handle, &leader).code == SLOPKIT_OK);

    // The child is a fork of this process, so a function of this binary is at
    // the same address and mapped in both.
    const auto address  = reinterpret_cast<std::uint64_t>(&process_state);
    const auto expected = *reinterpret_cast<const std::byte*>(&process_state);

    AccessMethod used = AccessMethod::none;
    const auto   read = session->read(address, 1, used);
    REQUIRE(read.has_value());
    REQUIRE(read->size() == 1);
    CHECK((*read)[0] == expected);
    CHECK_FALSE(slopkit::process::has_flag(used, AccessMethod::agent));

    // Module enumeration is agent-only, so a stopped target reports the reason
    // it cannot be served instead of a host-derived list.
    const auto modules = session->modules();
    REQUIRE_FALSE(modules.has_value());
    CHECK(modules.error() == AccessError::io_error);

    REQUIRE(vtable->debug_detach(handle).code == SLOPKIT_OK);
}

TEST_CASE("wine-proton reports clean errors", "[wine_proton]")
{
    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});
    slopkit::process::PluginAccess access(host);

    SECTION("nonexistent process")
    {
        const auto missing = access.attach(0x7fffffffu, "wine-proton");
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error() == AccessError::not_found);
    }

    SECTION("zero-length read")
    {
        auto self = access.attach(static_cast<ProcessId>(::getpid()), "wine-proton");
        REQUIRE(self.has_value());
        const auto result = self->read(0x1000, 0);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == AccessError::invalid_argument);
    }

    SECTION("a target the agent cannot serve")
    {
        auto self = access.attach(static_cast<ProcessId>(::getpid()), "wine-proton");
        REQUIRE(self.has_value());
        // The agent cannot serve a native target, so the read falls back to the
        // host path, which reports the unmapped address itself.
        const auto result = self->read(0, 16);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == AccessError::not_found);
    }
}

TEST_CASE("wine-proton rejects a stale handle after close_session", "[wine_proton]")
{
    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("wine-proton");
    REQUIRE(plugin != nullptr);

    auto session = plugin->open_session(static_cast<ProcessId>(::getpid()));
    REQUIRE(session.has_value());

    auto* vtable = session->vtable();
    void* handle = session->handle();
    REQUIRE(vtable != nullptr);

    REQUIRE(vtable->close_session(handle).code == SLOPKIT_OK);

    std::byte     byte {};
    std::size_t   read   = 0;
    std::uint32_t method = 0;
    CHECK(vtable->read_memory(handle, 0x1000, &byte, 1, &read, &method).code == SLOPKIT_ERR_INVALID_ARGUMENT);
}

TEST_CASE("wine-proton suspends and resumes its target", "[wine_proton]")
{
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    ChildGuard guard(child);
    if (child == 0)
    {
        for (;;)
        {
            ::pause();
        }
    }

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("wine-proton");
    REQUIRE(plugin != nullptr);

    auto session = plugin->open_session(static_cast<ProcessId>(child));
    REQUIRE(session.has_value());

    // The ABI 1.5 operations are present in the raw vtable.
    const auto* vtable = session->vtable();
    REQUIRE(vtable != nullptr);
    CHECK(vtable->suspend_target != nullptr);
    CHECK(vtable->resume_target != nullptr);
    CHECK(session->supports_suspend());

    REQUIRE(wait_for_stopped(static_cast<ProcessId>(child), false, std::chrono::seconds(2)));

    REQUIRE(session->suspend_target().has_value());
    CHECK(wait_for_stopped(static_cast<ProcessId>(child), true, std::chrono::seconds(2)));

    REQUIRE(session->resume_target().has_value());
    CHECK(wait_for_stopped(static_cast<ProcessId>(child), false, std::chrono::seconds(2)));
}

TEST_CASE("closing a suspended wine-proton session resumes the target", "[wine_proton]")
{
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    ChildGuard guard(child);
    if (child == 0)
    {
        for (;;)
        {
            ::pause();
        }
    }

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("wine-proton");
    REQUIRE(plugin != nullptr);

    {
        auto session = plugin->open_session(static_cast<ProcessId>(child));
        REQUIRE(session.has_value());
        REQUIRE(session->suspend_target().has_value());
        REQUIRE(wait_for_stopped(static_cast<ProcessId>(child), true, std::chrono::seconds(2)));
    }

    // close_session ran the session destructor, which must un-freeze the child.
    CHECK(wait_for_stopped(static_cast<ProcessId>(child), false, std::chrono::seconds(2)));
}

TEST_CASE("the injector environment drops process-local handoffs", "[wine_proton]")
{
    using slopkit::plugins::wine_proton::injector_environment;

    // A Proton/Wine target carries `WINESERVERSOCKET=<fd>`, an already-open
    // wineserver socket that only exists in the target's own descriptor table.
    // Inheriting it makes the injector's wine loader abort with "Bad server
    // socket <fd> : Bad file descriptor" instead of loading the agent.
    SECTION("a wineserver socket handed to the target is dropped")
    {
        const std::vector<std::string> environment {"PATH=/usr/bin",
                                                    "WINESERVERSOCKET=64",
                                                    "WINEPREFIX=/home/user/.steam/steam/steamapps/compatdata/1/pfx",
                                                    "DISPLAY=:0"};

        const auto filtered = injector_environment(environment);
        REQUIRE(filtered.size() == 3);
        CHECK(std::ranges::find(filtered, "WINESERVERSOCKET=64") == filtered.end());
        CHECK(std::ranges::find(filtered, "PATH=/usr/bin") != filtered.end());
        CHECK(std::ranges::find(filtered, "DISPLAY=:0") != filtered.end());
    }

    SECTION("a similarly named variable is kept")
    {
        const std::vector<std::string> environment {"WINESERVERSOCKET_BACKUP=1"};

        const auto filtered = injector_environment(environment);
        CHECK(filtered == environment);
    }

    SECTION("an environment without the handoff is unchanged")
    {
        const std::vector<std::string> environment {"PATH=/usr/bin", "HOME=/home/user"};

        CHECK(injector_environment(environment) == environment);
    }
}

TEST_CASE("the agent is staged where the target can reach it", "[wine_proton]")
{
    using slopkit::plugins::wine_proton::make_staging_directory;
    using slopkit::plugins::wine_proton::target_can_reach;

    const auto      base = std::filesystem::temp_directory_path() / "slopkit-staging-base";
    std::error_code error;
    std::filesystem::remove_all(base, error);

    SECTION("a target that shares the runtime directory is staged there")
    {
        REQUIRE(std::filesystem::create_directories(base, error));
        REQUIRE(::setenv("XDG_RUNTIME_DIR", base.c_str(), 1) == 0);

        const auto staged = make_staging_directory(static_cast<ProcessId>(::getpid()));
        REQUIRE(staged.has_value());
        CHECK(staged->parent_path() == base);
        CHECK(target_can_reach(static_cast<ProcessId>(::getpid()), *staged));

        ::unsetenv("XDG_RUNTIME_DIR");
        std::filesystem::remove_all(base, error);
        std::filesystem::remove_all(*staged, error);
    }

    SECTION("a containerised target is staged outside its private runtime directory")
    {
        // A Proton game only shares the temporary directory (and $HOME) with the
        // tool: its container replaces /run/user/<uid> with a private directory,
        // so nothing staged under the tool's XDG_RUNTIME_DIR was loadable and the
        // injector died in the loader with STATUS_NO_SUCH_FILE.
        REQUIRE(std::filesystem::create_directories(base / "hidden", error));

        const pid_t child = spawn_isolated_child(base);
        if (child < 0)
        {
            WARN("user namespaces are unavailable; the containerised staging case is skipped");
            std::filesystem::remove_all(base, error);
            return;
        }
        ChildGuard guard(child);

        // The host sees the directory, the target's namespace does not.
        REQUIRE(std::filesystem::exists(base / "hidden"));
        CHECK_FALSE(target_can_reach(static_cast<ProcessId>(child), base / "hidden"));

        REQUIRE(::setenv("XDG_RUNTIME_DIR", base.c_str(), 1) == 0);
        const auto staged = make_staging_directory(static_cast<ProcessId>(child));
        ::unsetenv("XDG_RUNTIME_DIR");

        REQUIRE(staged.has_value());
        CHECK_FALSE(staged->string().starts_with(base.string()));
        CHECK(target_can_reach(static_cast<ProcessId>(child), *staged));

        std::filesystem::remove_all(base, error);
        std::filesystem::remove_all(*staged, error);
    }

    SECTION("a process that is gone has no reachable directory")
    {
        CHECK_FALSE(target_can_reach(0x7fffffffu, "/"));
        const auto staged = make_staging_directory(0x7fffffffu);
        REQUIRE_FALSE(staged.has_value());
        CHECK(staged.error().find("no staging directory") != std::string::npos);
    }
}

TEST_CASE("a failed injector is reported as a useful line", "[wine_proton]")
{
    using slopkit::plugins::wine_proton::injector_failure_message;

    SECTION("the injector's own message wins over the surrounding traces")
    {
        // The target's own WINEDEBUG buries everything the loader prints, which
        // is why the first line of the capture was never the useful one.
        const std::string output = "8092.350:842c:8430:trace:seh:libc_addr_cb found signal trampoline\n"
                                   "slopkit-injector: the image \"C:\\game\\game.exe\" is not running\n"
                                   "8092.350:842c:8430:trace:seh:backtrace\n";
        CHECK(injector_failure_message(output) == "slopkit-injector: the image \"C:\\game\\game.exe\" is not running");
    }

    SECTION("without an injector message the last line is used")
    {
        // What the real failure printed: wine resolved the staged injector
        // through Z:, found nothing because the directory belonged to another
        // mount namespace, and fell back to its own launcher.
        const std::string output = "wine: created the configuration directory\n"
                                   "Application could not be started, or no application associated with the "
                                   "specified file.\n"
                                   "ShellExecuteEx failed: File not found.\n";
        CHECK(injector_failure_message(output) == "ShellExecuteEx failed: File not found.");
    }

    SECTION("an empty capture still has a reason")
    {
        CHECK(injector_failure_message("") == "no output");
    }
}

TEST_CASE("a paused target is not a failure for the agent hooks", "[wine_proton][agent-service]")
{
    using slopkit::platform::ForeignSignalPolicy;
    using slopkit::plugins::support::Session;
    using slopkit::plugins::wine_proton::AgentService;

    auto* page =
        static_cast<std::byte*>(::mmap(nullptr, kPageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(page != MAP_FAILED);
    const auto address = reinterpret_cast<std::uint64_t>(page);
    for (std::size_t index = 0; index < 8; ++index)
    {
        page[index] = static_cast<std::byte>(0x10 + index);
    }

    Session session(static_cast<ProcessId>(::getpid()), ForeignSignalPolicy::forward);
    session.suspended = true;

    // Our own SIGSTOP pause stops the agent's threads too, so a read answers
    // from the host path instead of timing out and disabling the agent.
    std::array<std::byte, 8> buffer {};
    const auto               read = AgentService::read(session, address, buffer);
    REQUIRE(read.has_value());
    CHECK(read->bytes == buffer.size());
    CHECK(std::equal(buffer.begin(), buffer.end(), page));

    // Allocation cannot run on a stopped target: a retryable busy error, never a
    // session-long disable.
    const auto alloc = AgentService::allocate(session, kPageSize, 0);
    REQUIRE_FALSE(alloc.has_value());
    CHECK(alloc.error().code == SLOPKIT_ERR_IO);
    const auto freed = AgentService::free(session, address);
    REQUIRE_FALSE(freed.has_value());
    CHECK(freed.error().code == SLOPKIT_ERR_IO);

    // With no agent-derived list recorded yet, modules reports the pause rather
    // than a host-derived list.
    const auto modules = AgentService::modules(session);
    REQUIRE_FALSE(modules.has_value());

    // Resume: the same session keeps serving, so the pause disabled nothing.
    session.suspended  = false;
    const auto resumed = AgentService::read(session, address, buffer);
    REQUIRE(resumed.has_value());
    CHECK(std::equal(buffer.begin(), buffer.end(), page));

    ::munmap(page, kPageSize);
}
