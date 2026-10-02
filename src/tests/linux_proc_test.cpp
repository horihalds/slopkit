#include <catch2/catch.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <signal.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "plugin/plugin_host.hpp"
#include "process/plugin_access.hpp"

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
} // namespace

TEST_CASE("linux-proc is discovered with the expected metadata", "[linux_proc]")
{
    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    const auto* plugin = host.find("linux-proc");
    REQUIRE(plugin != nullptr);
    CHECK(plugin->precedence() == 100);
    CHECK(slopkit::process::has_flag(plugin->access_methods(), AccessMethod::process_vm));
    CHECK(slopkit::process::has_flag(plugin->access_methods(), AccessMethod::procfs_mem));
}

TEST_CASE("linux-proc lists the current process", "[linux_proc]")
{
    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});
    slopkit::process::PluginAccess access(host);

    const auto processes = access.list_processes();
    REQUIRE(processes.has_value());

    const auto self  = static_cast<ProcessId>(::getpid());
    bool       found = false;
    for (const auto& process : *processes)
    {
        if (process.pid == self)
        {
            found = true;
            CHECK(process.plugin_id == "linux-proc");
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("linux-proc lists modules, threads and regions of the current process", "[linux_proc]")
{
    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});
    slopkit::process::PluginAccess access(host);

    auto session = access.attach(static_cast<ProcessId>(::getpid()), "linux-proc");
    REQUIRE(session.has_value());

    const auto modules = session->modules();
    REQUIRE(modules.has_value());
    CHECK_FALSE(modules->empty());

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
    CHECK(std::ranges::any_of(*regions,
                              [](const auto& region)
                              {
                                  return region.readable;
                              }));
}

TEST_CASE("linux-proc reads and writes target memory", "[linux_proc]")
{
    int pipe_fds[2];
    REQUIRE(::pipe(pipe_fds) == 0);

    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    ChildGuard guard(child);

    if (child == 0)
    {
        ::close(pipe_fds[0]);
        void* page = ::mmap(nullptr, kPageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED)
        {
            ::_exit(1);
        }
        auto* bytes = static_cast<std::byte*>(page);
        for (std::size_t i = 0; i < kPageSize; ++i)
        {
            bytes[i] = static_cast<std::byte>(i % 251);
        }
        const auto address = reinterpret_cast<std::uint64_t>(page);
        (void)::write(pipe_fds[1], &address, sizeof(address));
        ::close(pipe_fds[1]);
        for (;;)
        {
            ::pause();
        }
    }

    ::close(pipe_fds[1]);
    std::uint64_t address = 0;
    REQUIRE(::read(pipe_fds[0], &address, sizeof(address)) == static_cast<ssize_t>(sizeof(address)));
    ::close(pipe_fds[0]);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});
    slopkit::process::PluginAccess access(host);

    auto session = access.attach(static_cast<ProcessId>(child), "linux-proc");
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
}

TEST_CASE("linux-proc reports clean errors", "[linux_proc]")
{
    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});
    slopkit::process::PluginAccess access(host);

    SECTION("nonexistent process")
    {
        const auto missing = access.attach(0x7fffffffu, "linux-proc");
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error() == AccessError::not_found);
    }

    SECTION("zero-length read")
    {
        auto self = access.attach(static_cast<ProcessId>(::getpid()), "linux-proc");
        REQUIRE(self.has_value());
        const auto result = self->read(0x1000, 0);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == AccessError::invalid_argument);
    }

    SECTION("unmapped address")
    {
        auto self = access.attach(static_cast<ProcessId>(::getpid()), "linux-proc");
        REQUIRE(self.has_value());
        const auto result = self->read(0, 16);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == AccessError::not_found);
    }

    SECTION("another user's process is denied")
    {
        if (::geteuid() == 0)
        {
            SUCCEED("running as root; skipping the permission check");
            return;
        }

        auto init = access.attach(1, "linux-proc");
        if (!init.has_value())
        {
            CHECK(init.error() == AccessError::not_found);
            return;
        }

        const auto result = init->read(0x400000, 32);
        REQUIRE_FALSE(result.has_value());
        const auto error = result.error();
        CHECK((error == AccessError::permission_denied || error == AccessError::not_found));
    }
}
