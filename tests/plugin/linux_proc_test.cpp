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

#include "platform/linux/memory.hpp"
#include "platform/linux/procfs.hpp"
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

    // modules_from_maps sorts by base, so the first non-anonymous module with a
    // path is the executable and its entry point must land inside the image.
    const auto main_module =
        std::ranges::find_if(*modules,
                             [](const slopkit::process::ModuleInfo& module)
                             {
                                 return module.kind != slopkit::process::ModuleKind::anonymous && !module.path.empty();
                             });
    REQUIRE(main_module != modules->end());
    CHECK(main_module->entry != 0);
    CHECK(main_module->entry >= main_module->base);

    // Exactly one module is flagged as the main image, and it backs the exe.
    const auto flagged = std::ranges::count_if(*modules,
                                               [](const slopkit::process::ModuleInfo& module)
                                               {
                                                   return module.is_main;
                                               });
    CHECK(flagged == 1);

    const auto main_image = std::ranges::find_if(*modules,
                                                 [](const slopkit::process::ModuleInfo& module)
                                                 {
                                                     return module.is_main;
                                                 });
    REQUIRE(main_image != modules->end());
    const auto exe = slopkit::platform::read_exe(static_cast<ProcessId>(::getpid()));
    REQUIRE(exe.has_value());
    CHECK(main_image->path == *exe);
    CHECK(main_image->entry != 0);

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

    // read_into writes straight into the caller buffer through the cached
    // descriptor, and reports the same bytes as read().
    std::array<std::byte, 16> into {};
    const auto                into_count = session->read_into(address + 64, into);
    REQUIRE(into_count.has_value());
    REQUIRE(*into_count == into.size());
    for (std::size_t i = 0; i < into.size(); ++i)
    {
        REQUIRE(into[i] == static_cast<std::byte>((64 + i) % 251));
    }

    CHECK(session->last_method() != AccessMethod::none);
}

TEST_CASE("the cached descriptor path reports like the one-shot helper", "[linux_proc]")
{
    auto* page =
        static_cast<std::byte*>(::mmap(nullptr, kPageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(page != MAP_FAILED);
    for (std::size_t i = 0; i < kPageSize; ++i)
    {
        page[i] = static_cast<std::byte>(i % 251);
    }
    const auto address = reinterpret_cast<std::uint64_t>(page);

    slopkit::platform::MemAccess access(static_cast<ProcessId>(::getpid()));

    std::array<std::byte, 64> buffer {};
    const auto                result = access.read(address + 128, buffer);
    REQUIRE(result.has_value());
    REQUIRE(result->bytes == buffer.size());
    for (std::size_t i = 0; i < buffer.size(); ++i)
    {
        REQUIRE(buffer[i] == static_cast<std::byte>((128 + i) % 251));
    }

    // The cached-descriptor fallback reports an unmapped address as unmapped.
    std::array<std::byte, 8> none {};
    const auto               missing = access.read(0x1, none);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == slopkit::platform::MemoryError::unmapped);

    // The one-shot helper keeps reporting the same result.
    const auto oneshot = slopkit::platform::read_memory(static_cast<ProcessId>(::getpid()), address + 128, none);
    REQUIRE(oneshot.has_value());
    REQUIRE(oneshot->bytes == none.size());
    CHECK(none[0] == static_cast<std::byte>((128) % 251));

    ::munmap(page, kPageSize);
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
