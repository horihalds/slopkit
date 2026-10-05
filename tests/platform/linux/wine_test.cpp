#include <catch2/catch.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "platform/linux/wine.hpp"
#include "plugin/plugin_host.hpp"
#include "process/plugin_access.hpp"

namespace
{
    using slopkit::platform::WineFlavor;
    using slopkit::process::ProcessId;

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

    bool wait_for_environ(pid_t pid, std::string_view needle)
    {
        for (int attempt = 0; attempt < 200; ++attempt)
        {
            for (const auto& entry : slopkit::platform::read_environ(static_cast<ProcessId>(pid)))
            {
                if (entry.find(needle) != std::string::npos)
                {
                    return true;
                }
            }
            ::usleep(5000);
        }
        return false;
    }
} // namespace

TEST_CASE("wine detection classifies synthetic processes", "[wine]")
{
    SECTION("a native Linux process is not claimed")
    {
        const std::vector<std::string> environ {"PATH=/usr/bin", "HOME=/home/user"};
        const std::vector<std::string> cmdline {"/usr/bin/bash", "-l"};
        const std::string              maps = "561000000000-561000001000 r--p 00000000 08:01 100 /usr/bin/bash\n"
                                              "7f0000000000-7f0000001000 r-xp 00000000 08:01 200 /usr/lib/libc.so.6\n";

        const auto result = slopkit::platform::classify_wine_process(environ, cmdline, maps);
        CHECK_FALSE(result.claimed());
        CHECK(result.flavor == WineFlavor::none);
    }

    SECTION("a Wine process is claimed as wine")
    {
        const std::vector<std::string> environ {"WINEPREFIX=/home/user/.wine", "DISPLAY=:0"};
        const std::vector<std::string> cmdline {"C:\\windows\\system32\\winedevice.exe"};
        const std::string              maps = "7f0000000000-7f0000001000 r-xp 00000000 08:01 300 "
                                              "/home/user/.wine/drive_c/windows/system32/ntdll.dll\n";

        const auto result = slopkit::platform::classify_wine_process(environ, cmdline, maps);
        REQUIRE(result.claimed());
        CHECK(result.flavor == WineFlavor::wine);
        CHECK_FALSE(result.evidence.details.empty());
    }

    SECTION("a Proton process is claimed as proton")
    {
        const std::vector<std::string> environ {"STEAM_COMPAT_DATA_PATH=/home/user/.steam/steamapps/compatdata/123",
                                                "PROTON_LOG=1"};
        const std::vector<std::string> cmdline {
            "/home/user/.steam/steamapps/common/Proton 9.0/proton", "waitforexitandrun", "game.exe"};
        const std::string maps = "7f0000000000-7f0000001000 r-xp 00000000 08:01 300 /usr/lib/libc.so.6\n";

        const auto result = slopkit::platform::classify_wine_process(environ, cmdline, maps);
        REQUIRE(result.claimed());
        CHECK(result.flavor == WineFlavor::proton);
    }

    SECTION("Proton is detected from a single wrapper signal")
    {
        const std::vector<std::string> environ {};
        const std::vector<std::string> cmdline {"/usr/bin/pressure-vessel", "--"};
        const std::string              maps = "7f0000000000-7f0000001000 r-xp 00000000 08:01 300 /usr/lib/libc.so.6\n";

        const auto result = slopkit::platform::classify_wine_process(environ, cmdline, maps);
        REQUIRE(result.claimed());
        CHECK(result.flavor == WineFlavor::proton);
    }

    SECTION("Wine is detected from a mapping-only signal")
    {
        const std::vector<std::string> environ {};
        const std::vector<std::string> cmdline {"/home/user/game"};
        const std::string              maps = "7f0000000000-7f0000001000 r-xp 00000000 08:01 400 "
                                              "/usr/lib/x86_64-linux-gnu/wine/x86_64-unix/ntdll.so\n";

        const auto result = slopkit::platform::classify_wine_process(environ, cmdline, maps);
        REQUIRE(result.claimed());
        CHECK(result.flavor == WineFlavor::wine);
        CHECK(result.evidence.maps_signal);
    }

    SECTION("mapped ELF libraries do not make a process Wine")
    {
        const std::vector<std::string> environ {};
        const std::vector<std::string> cmdline {"/usr/bin/bash"};
        const std::string              maps = "7f0000000000-7f0000001000 r-xp 00000000 08:01 500 /usr/lib/libm.so.6\n";

        const auto result = slopkit::platform::classify_wine_process(environ, cmdline, maps);
        CHECK_FALSE(result.claimed());
    }
}

TEST_CASE("both bundled plugins are discovered in precedence order", "[wine]")
{
    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    REQUIRE(host.plugins().size() == 2);
    CHECK(host.plugins().front()->id() == "wine-proton");
    CHECK(host.plugins().front()->precedence() == 10);
    CHECK(host.plugins().back()->id() == "linux-proc");
    CHECK(host.plugins().back()->precedence() == 100);
}

TEST_CASE("wine-proton wins over linux-proc for a detected process", "[wine]")
{
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    ChildGuard guard(child);

    if (child == 0)
    {
        // Exec a plain process with a Wine-looking environment so both plugins
        // claim it.
        char  arg0[] = "sleep";
        char  arg1[] = "30";
        char* argv[] = {arg0, arg1, nullptr};
        char  env0[] = "PATH=/usr/bin:/bin";
        char  env1[] = "WINEPREFIX=/tmp/slopkit-wine";
        char* envp[] = {env0, env1, nullptr};
        ::execve("/bin/sleep", argv, envp);
        ::_exit(127);
    }

    REQUIRE(wait_for_environ(child, "WINEPREFIX"));

    const auto detection = slopkit::platform::classify_wine_process(static_cast<ProcessId>(child));
    REQUIRE(detection.claimed());

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

    const auto& claimants = entry->claimants;
    CHECK(std::find(claimants.begin(), claimants.end(), "wine-proton") != claimants.end());
    CHECK(std::find(claimants.begin(), claimants.end(), "linux-proc") != claimants.end());
}
