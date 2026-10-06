#include <catch2/catch.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <sys/mman.h>
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
