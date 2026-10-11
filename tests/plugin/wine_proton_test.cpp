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

    bool write_control_file(const std::string& path, const std::string& value)
    {
        std::ofstream file(path, std::ios::trunc);
        if (file)
        {
            file << value;
        }
        return file.good();
    }

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

    // A minimal PE header (kind and bitness) so the guest-image walk can read a
    // mapped file's image kind.
    std::vector<std::byte> make_pe(bool is_dll)
    {
        constexpr std::size_t   lfanew              = 0x40;
        constexpr std::size_t   characteristics_off = 22;
        constexpr std::uint16_t image_file_dll      = 0x2000;
        constexpr std::uint16_t machine_amd64       = 0x8664;

        std::vector<std::byte> bytes(lfanew + 44, std::byte {0});
        bytes[0]          = std::byte {'M'};
        bytes[1]          = std::byte {'Z'};
        bytes[0x3c]       = static_cast<std::byte>(lfanew & 0xff);
        bytes[0x3d]       = static_cast<std::byte>((lfanew >> 8) & 0xff);
        bytes[lfanew]     = std::byte {'P'};
        bytes[lfanew + 1] = std::byte {'E'};

        bytes[lfanew + 4] = static_cast<std::byte>(machine_amd64 & 0xff);
        bytes[lfanew + 5] = static_cast<std::byte>((machine_amd64 >> 8) & 0xff);

        const auto characteristics              = static_cast<std::uint16_t>(is_dll ? image_file_dll : 0);
        bytes[lfanew + characteristics_off]     = static_cast<std::byte>(characteristics & 0xff);
        bytes[lfanew + characteristics_off + 1] = static_cast<std::byte>((characteristics >> 8) & 0xff);
        return bytes;
    }

    void write_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    // A child that maps `image` read/exec at offset 0 and pauses, so its maps
    // carry a file-backed image the guest-image walk can pick up.
    pid_t spawn_mapping_child(const std::filesystem::path& image)
    {
        const pid_t child = ::fork();
        if (child != 0)
        {
            return child;
        }
        const int descriptor = ::open(image.c_str(), O_RDONLY);
        if (descriptor < 0)
        {
            ::_exit(70);
        }
        if (::mmap(nullptr, kPageSize, PROT_READ | PROT_EXEC, MAP_PRIVATE, descriptor, 0) == MAP_FAILED)
        {
            ::_exit(71);
        }
        for (;;)
        {
            ::pause();
        }
    }

    bool wait_for_mapping(ProcessId pid, const std::string& path)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;)
        {
            for (const auto& region : slopkit::platform::read_maps(pid))
            {
                if (region.path == path)
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

    bool wait_for_wine(ProcessId pid)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;)
        {
            if (slopkit::platform::classify_wine_process(pid).claimed())
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
}

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

TEST_CASE("a wine target's guest image name is its executable's file name", "[wine_proton]")
{
    const auto dir = std::filesystem::temp_directory_path() / ("slopkit-wine-image-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    const auto game_dir = dir / "wine" / "drive_c" / "game";
    std::filesystem::create_directories(game_dir);

    const auto game_path = game_dir / "game.exe";
    const auto dll_path  = game_dir / "game.dll";
    write_bytes(game_path, make_pe(false));
    write_bytes(dll_path, make_pe(true));

    SECTION("the executable's file name keeps the extension")
    {
        const pid_t child = spawn_mapping_child(game_path);
        REQUIRE(child >= 0);
        ChildGuard guard(child);
        REQUIRE(wait_for_mapping(static_cast<ProcessId>(child), game_path.string()));

        const auto name = slopkit::plugins::wine_proton::guest_image_name(static_cast<ProcessId>(child));
        REQUIRE(name.has_value());
        CHECK(*name == "game.exe");
    }

    SECTION("a DLL mapping is not an executable image")
    {
        const pid_t child = spawn_mapping_child(dll_path);
        REQUIRE(child >= 0);
        ChildGuard guard(child);
        REQUIRE(wait_for_mapping(static_cast<ProcessId>(child), dll_path.string()));

        CHECK_FALSE(slopkit::plugins::wine_proton::guest_image_name(static_cast<ProcessId>(child)).has_value());
    }

    SECTION("a process with no PE image has no guest name")
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

        CHECK_FALSE(slopkit::plugins::wine_proton::guest_image_name(static_cast<ProcessId>(child)).has_value());
    }

    std::filesystem::remove_all(dir);
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

    const auto regions = session->regions();
    REQUIRE(regions.has_value());
    CHECK_FALSE(regions->empty());
}

TEST_CASE("wine-proton serves reads and writes from the host path", "[wine_proton]")
{
    auto* page =
        static_cast<std::byte*>(::mmap(nullptr, kPageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(page != MAP_FAILED);
    const auto address = reinterpret_cast<std::uint64_t>(page);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});
    slopkit::process::PluginAccess access(host);

    auto session = access.attach(static_cast<ProcessId>(::getpid()), "wine-proton");
    REQUIRE(session.has_value());

    // Reads and writes never round-trip through the resident agent: the host
    // process_vm path (or procfs) answers them directly.
    const auto data = session->read(address, 64);
    REQUIRE(data.has_value());
    CHECK(data->size() == 64);
    CHECK((slopkit::process::has_flag(session->last_method(), AccessMethod::process_vm)
           || slopkit::process::has_flag(session->last_method(), AccessMethod::procfs_mem)));
    CHECK_FALSE(slopkit::process::has_flag(session->last_method(), AccessMethod::agent));

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
    CHECK_FALSE(slopkit::process::has_flag(session->last_method(), AccessMethod::agent));

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

TEST_CASE("a paused target keeps the agent-only hooks unavailable", "[wine_proton][agent-service]")
{
    using slopkit::platform::ForeignSignalPolicy;
    using slopkit::plugins::support::Session;
    using slopkit::plugins::wine_proton::AgentService;

    Session session(static_cast<ProcessId>(::getpid()), ForeignSignalPolicy::forward);
    session.suspended = true;

    // Our own SIGSTOP pause stops the agent's threads too, so allocation and the
    // introspection walks cannot be served and report the error instead.
    const auto alloc = AgentService::allocate(session, kPageSize, 0);
    REQUIRE_FALSE(alloc.has_value());
    CHECK(alloc.error().code == SLOPKIT_ERR_IO);
    const auto freed = AgentService::free(session, 0x1000);
    REQUIRE_FALSE(freed.has_value());
    CHECK(freed.error().code == SLOPKIT_ERR_IO);

    const auto modules = AgentService::modules(session);
    REQUIRE_FALSE(modules.has_value());
}

TEST_CASE("the name cache remembers a claimed pid for its TTL", "[wine_proton]")
{
    using slopkit::plugins::wine_proton::NameCache;

    const auto now = std::chrono::steady_clock::now();
    NameCache  cache(std::chrono::seconds(5));

    SECTION("a remembered name is returned inside the TTL")
    {
        cache.remember(42, "game.exe", now);

        const auto name = cache.lookup(42, now + std::chrono::seconds(4));
        REQUIRE(name.has_value());
        CHECK(*name == "game.exe");
    }

    SECTION("an unknown pid has no name")
    {
        CHECK_FALSE(cache.lookup(7, now).has_value());
    }

    SECTION("an entry is dropped once it expires")
    {
        cache.remember(42, "game.exe", now);

        CHECK_FALSE(cache.lookup(42, now + std::chrono::seconds(5)).has_value());
        CHECK_FALSE(cache.lookup(42, now + std::chrono::seconds(6)).has_value());
    }

    SECTION("forget removes an entry immediately")
    {
        cache.remember(42, "game.exe", now);
        cache.forget(42);

        CHECK_FALSE(cache.lookup(42, now).has_value());
    }

    SECTION("a newer name replaces the remembered one")
    {
        cache.remember(42, "old.exe", now);
        cache.remember(42, "new.exe", now + std::chrono::seconds(1));

        const auto name = cache.lookup(42, now + std::chrono::seconds(2));
        REQUIRE(name.has_value());
        CHECK(*name == "new.exe");
    }

    SECTION("the map stays bounded")
    {
        // All entries share one expiry, so the emplace order breaks the tie and
        // the lowest pid is evicted first.
        constexpr ProcessId kOverCapacity = NameCache::kCapacity + 1;
        for (ProcessId pid = 1; pid <= kOverCapacity; ++pid)
        {
            cache.remember(pid, "game.exe", now);
        }

        CHECK_FALSE(cache.lookup(1, now).has_value());
        CHECK(cache.lookup(kOverCapacity, now).has_value());
    }
}

TEST_CASE("a claimed wine target is listed by its guest executable name", "[wine_proton]")
{
    const auto dir = std::filesystem::temp_directory_path() / ("slopkit-wine-list-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    const auto game_dir = dir / "wine" / "drive_c" / "game";
    std::filesystem::create_directories(game_dir);

    const auto game_path = game_dir / "game.exe";
    write_bytes(game_path, make_pe(false));

    // A forked child keeps the parent's maps, so only the extra game.exe makes
    // it look like a wine target.
    const pid_t child = spawn_mapping_child(game_path);
    REQUIRE(child >= 0);
    ChildGuard guard(child);
    REQUIRE(wait_for_mapping(static_cast<ProcessId>(child), game_path.string()));
    REQUIRE(wait_for_wine(static_cast<ProcessId>(child)));

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});
    slopkit::process::PluginAccess access(host);

    const auto processes = access.list_processes();
    REQUIRE(processes.has_value());

    const slopkit::process::ProcessInfo* entry = nullptr;
    for (const auto& process : *processes)
    {
        if (process.pid == static_cast<ProcessId>(child))
        {
            entry = &process;
            break;
        }
    }

    REQUIRE(entry != nullptr);
    CHECK(entry->name == "game.exe");
    CHECK(entry->plugin_id == "wine-proton");
    CHECK(std::find(entry->claimants.begin(), entry->claimants.end(), "wine-proton") != entry->claimants.end());
    CHECK(std::find(entry->claimants.begin(), entry->claimants.end(), "linux-proc") != entry->claimants.end());

    std::filesystem::remove_all(dir);
}

TEST_CASE("a claimed wine process without a PE image keeps its loader name", "[wine_proton]")
{
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    ChildGuard guard(child);
    if (child == 0)
    {
        char  arg0[] = "sleep";
        char  arg1[] = "30";
        char* argv[] = {arg0, arg1, nullptr};
        char  env0[] = "PATH=/usr/bin:/bin";
        char  env1[] = "WINEPREFIX=/tmp/slopkit-wine";
        char* envp[] = {env0, env1, nullptr};
        ::execve("/bin/sleep", argv, envp);
        ::_exit(127);
    }
    REQUIRE(wait_for_wine(static_cast<ProcessId>(child)));

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});
    slopkit::process::PluginAccess access(host);

    const auto processes = access.list_processes();
    REQUIRE(processes.has_value());

    const slopkit::process::ProcessInfo* entry = nullptr;
    for (const auto& process : *processes)
    {
        if (process.pid == static_cast<ProcessId>(child))
        {
            entry = &process;
            break;
        }
    }

    REQUIRE(entry != nullptr);
    CHECK(entry->plugin_id == "wine-proton");
    CHECK_FALSE(entry->name.empty());
    CHECK(entry->name == "sleep");
}
