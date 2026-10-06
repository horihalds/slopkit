#include <catch2/catch.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>
#include <utility>

#include <signal.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "platform/linux/procfs.hpp"
#include "platform/linux/wine.hpp"
#include "plugin/plugin_host.hpp"
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

TEST_CASE("wine-proton lists modules, threads and regions of a session", "[wine_proton]")
{
    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});
    slopkit::process::PluginAccess access(host);

    const auto self    = static_cast<ProcessId>(::getpid());
    auto       session = access.attach(self, "wine-proton");
    REQUIRE(session.has_value());

    // Wine only surfaces PE images, so a native process legitimately has none;
    // every module it does return must be a PE image.
    const auto modules = session->modules();
    REQUIRE(modules.has_value());
    for (const auto& module : *modules)
    {
        CHECK(module.kind == slopkit::process::ModuleKind::pe);
    }

    const auto threads = session->threads();
    REQUIRE(threads.has_value());
    CHECK_FALSE(threads->empty());

    const auto regions = session->regions();
    REQUIRE(regions.has_value());
    CHECK_FALSE(regions->empty());
    for (const auto& region : *regions)
    {
        CHECK(region.start < region.end);
    }
}

TEST_CASE("wine-proton reads and writes target memory", "[wine_proton]")
{
    auto* page =
        static_cast<std::byte*>(::mmap(nullptr, kPageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(page != MAP_FAILED);
    for (std::size_t i = 0; i < kPageSize; ++i)
    {
        page[i] = static_cast<std::byte>(i % 251);
    }
    const auto address = reinterpret_cast<std::uint64_t>(page);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});
    slopkit::process::PluginAccess access(host);

    auto session = access.attach(static_cast<ProcessId>(::getpid()), "wine-proton");
    REQUIRE(session.has_value());

    const auto data = session->read(address, 64);
    REQUIRE(data.has_value());
    REQUIRE(data->size() == 64);
    for (std::size_t i = 0; i < 64; ++i)
    {
        REQUIRE((*data)[i] == static_cast<std::byte>(i % 251));
    }

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
    REQUIRE(*written == pattern.size());

    const auto readback = session->read(address, pattern.size());
    REQUIRE(readback.has_value());
    REQUIRE(readback->size() == pattern.size());
    REQUIRE(std::equal(readback->begin(), readback->end(), pattern.begin()));

    CHECK(session->last_method() != AccessMethod::none);

    ::munmap(page, kPageSize);
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

    SECTION("unmapped address")
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
