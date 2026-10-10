#include <catch2/catch.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "platform/linux/procfs.hpp"
#include "plugin/plugin.hpp"
#include "plugin/plugin_host.hpp"
#include "process/types.hpp"

namespace
{
    // A shared counter, so the parent can tell the child kept running after an
    // allocation: the child's own copy of a private variable would be invisible.
    class AllocChild
    {
    public:
        AllocChild()
        {
            counter_ = static_cast<volatile std::uint64_t*>(
                ::mmap(nullptr, sizeof(std::uint64_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
            if (counter_ == MAP_FAILED)
            {
                counter_ = nullptr;
                return;
            }
            *counter_ = 0;

            const pid_t pid = ::fork();
            if (pid == 0)
            {
                for (;;)
                {
                    *counter_ = *counter_ + 1;
                }
                ::_exit(0);
            }
            if (pid > 0)
            {
                pid_ = pid;
            }
        }

        ~AllocChild()
        {
            if (pid_ > 0)
            {
                ::kill(pid_, SIGKILL);
                ::waitpid(pid_, nullptr, 0);
            }
            if (counter_ != nullptr)
            {
                ::munmap(const_cast<std::uint64_t*>(counter_), sizeof(std::uint64_t));
            }
        }

        AllocChild(const AllocChild&)            = delete;
        AllocChild& operator=(const AllocChild&) = delete;

        [[nodiscard]] std::uint32_t pid() const
        {
            return static_cast<std::uint32_t>(pid_);
        }

        [[nodiscard]] std::uint64_t counter() const
        {
            return counter_ != nullptr ? *counter_ : 0;
        }

    private:
        pid_t                   pid_ {-1};
        volatile std::uint64_t* counter_ {nullptr};
    };

    // Returns the region that fully contains [address, address + size), if one
    // exists. Linux `mmap` merges a new anonymous mapping into an adjacent one
    // when their protection and backing match, so two neighbouring pages can
    // appear as a single region that starts at either base; a mapping is found
    // by covering its range, never by a region starting exactly at its base.
    // Whole-page ranges stay exact: VMA boundaries are page-aligned, so a page
    // can never straddle two regions.
    std::optional<slopkit::platform::MappedRegion>
    region_covering(std::uint32_t pid, std::uint64_t address, std::uint64_t size)
    {
        for (const auto& region : slopkit::platform::read_maps(pid))
        {
            if (region.start <= address && address + size <= region.end)
            {
                return region;
            }
        }
        return std::nullopt;
    }

    bool mapped(std::uint32_t pid, std::uint64_t address, std::uint64_t size)
    {
        return region_covering(pid, address, size).has_value();
    }

    // True when `pid` has a region covering [address, address + size) that is
    // readable, writable and executable. Coverage, not an exact VMA start, is
    // used for the same coalescing reason as `region_covering`.
    bool mapped_rwx(std::uint32_t pid, std::uint64_t address, std::uint64_t size)
    {
        const auto region = region_covering(pid, address, size);
        return region.has_value() && region->readable && region->writable && region->executable;
    }

    void wait_for_progress(const AllocChild& child)
    {
        const std::uint64_t before = child.counter();
        for (int i = 0; i < 200; ++i)
        {
            if (child.counter() != before)
            {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds {5});
        }
        FAIL("the target stopped making progress");
    }
} // namespace

TEST_CASE("linux-proc maps and unmaps target memory through the ABI", "[linux_proc][alloc]")
{
    AllocChild child;
    REQUIRE(child.pid() != 0);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("linux-proc");
    REQUIRE(plugin != nullptr);

    auto session = plugin->open_session(child.pid());
    REQUIRE(session.has_value());
    REQUIRE(session->supports_allocation());

    constexpr std::size_t page = 0x1000;

    // A hint-less mapping lands page-aligned and read/write/execute-able.
    auto allocated = session->allocate_memory(page, 0);
    REQUIRE(allocated.has_value());
    const std::uint64_t address = *allocated;
    CHECK((address % page) == 0);
    CHECK(mapped_rwx(child.pid(), address, page));

    // The mapping is real memory: bytes written through the plugin read back.
    const std::vector<std::byte>   payload {std::byte {0xDE}, std::byte {0xAD}, std::byte {0xBE}, std::byte {0xEF}};
    slopkit::process::AccessMethod method  = slopkit::process::AccessMethod::none;
    auto                           written = session->write(address, payload, method);
    REQUIRE(written.has_value());
    CHECK(*written == payload.size());

    auto read = session->read(address, 4, method);
    REQUIRE(read.has_value());
    CHECK(*read == payload);

    // The target itself keeps running across the whole sequence.
    wait_for_progress(child);

    // Freeing removes exactly that mapping.
    auto freed = session->free_memory(address);
    REQUIRE(freed.has_value());
    CHECK_FALSE(mapped(child.pid(), address, page));
    wait_for_progress(child);

    // Freeing it again reports not-found; a foreign address (a module base) is
    // never unmapped.
    auto twice = session->free_memory(address);
    REQUIRE_FALSE(twice.has_value());
    CHECK(twice.error() == slopkit::process::AccessError::not_found);

    auto modules = session->modules();
    REQUIRE(modules.has_value());
    REQUIRE(!modules->empty());
    auto foreign = session->free_memory(modules->front().base);
    REQUIRE_FALSE(foreign.has_value());
    CHECK(foreign.error() == slopkit::process::AccessError::not_found);
    CHECK(mapped(child.pid(), modules->front().base, page));

    // Zero size and a refused mapping are reported, never a bogus address.
    auto zero = session->allocate_memory(0, 0);
    REQUIRE_FALSE(zero.has_value());
    CHECK(zero.error() == slopkit::process::AccessError::invalid_argument);
}

TEST_CASE("linux-proc honours a best-effort allocation hint", "[linux_proc][alloc]")
{
    AllocChild child;
    REQUIRE(child.pid() != 0);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("linux-proc");
    REQUIRE(plugin != nullptr);

    auto session = plugin->open_session(child.pid());
    REQUIRE(session.has_value());

    // Take a mapping, free it, then ask for the very same address: it is free
    // again, so the hint is honoured exactly.
    auto first = session->allocate_memory(0x2000, 0);
    REQUIRE(first.has_value());
    REQUIRE(session->free_memory(*first).has_value());

    auto hinted = session->allocate_memory(0x2000, *first);
    REQUIRE(hinted.has_value());
    CHECK(*hinted == *first);
    REQUIRE(session->free_memory(*hinted).has_value());
}

TEST_CASE("linux-proc maps near a hint that is already taken", "[linux_proc][alloc]")
{
    AllocChild child;
    REQUIRE(child.pid() != 0);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("linux-proc");
    REQUIRE(plugin != nullptr);

    auto session = plugin->open_session(child.pid());
    REQUIRE(session.has_value());
    REQUIRE(session->supports_allocation());

    constexpr std::size_t page = 0x1000;

    // Hold a mapping so the page at its base is taken. Hinting it must land in
    // the closest free page instead of giving up and mapping anywhere.
    auto taken = session->allocate_memory(page, 0);
    REQUIRE(taken.has_value());
    REQUIRE(mapped(child.pid(), *taken, page));

    // Fault the hinted page in and leave a marker in it. The nearest free page
    // abuts this one, so the kernel usually merges the new mapping into a
    // single region starting at either base — which is why the checks here are
    // coverage-based. Reading the marker back after the hinted allocation
    // proves the hinted page's contents survived, without relying on a VMA
    // boundary.
    const std::vector<std::byte>   payload {std::byte {0xDE}, std::byte {0xAD}, std::byte {0xBE}, std::byte {0xEF}};
    slopkit::process::AccessMethod method = slopkit::process::AccessMethod::none;
    auto                           marker = session->write(*taken, payload, method);
    REQUIRE(marker.has_value());
    CHECK(*marker == payload.size());

    auto near = session->allocate_memory(page, *taken);
    REQUIRE(near.has_value());
    const std::uint64_t address = *near;
    CHECK((address % page) == 0);
    CHECK(address != *taken);
    CHECK(mapped_rwx(child.pid(), address, page));

    // Close enough for the ±2 GB a near jump reaches (in practice a few pages).
    const std::uint64_t distance = address > *taken ? address - *taken : *taken - address;
    CHECK(distance <= 0x80000000ull);

    // The hinted region itself is untouched: its marker still reads back.
    CHECK(mapped(child.pid(), *taken, page));
    auto read = session->read(*taken, payload.size(), method);
    REQUIRE(read.has_value());
    CHECK(*read == payload);

    REQUIRE(session->free_memory(address).has_value());
    REQUIRE(session->free_memory(*taken).has_value());
}

TEST_CASE("a debug session and an allocation never overlap", "[linux_proc][alloc]")
{
    AllocChild child;
    REQUIRE(child.pid() != 0);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("linux-proc");
    REQUIRE(plugin != nullptr);

    auto session = plugin->open_session(child.pid());
    REQUIRE(session.has_value());

    auto* vtable = session->vtable();
    void* handle = session->handle();
    REQUIRE(vtable != nullptr);
    REQUIRE(vtable->debug_attach != nullptr);

    std::uint32_t leader = 0;
    REQUIRE(vtable->debug_attach(handle, &leader).code == SLOPKIT_OK);

    // The debugger owns the thread group, so allocation is refused.
    auto refused = session->allocate_memory(0x1000, 0);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == slopkit::process::AccessError::permission_denied);

    REQUIRE(vtable->debug_detach(handle).code == SLOPKIT_OK);

    // Once the debugger lets go, allocation works again.
    auto allocated = session->allocate_memory(0x1000, 0);
    REQUIRE(allocated.has_value());
    REQUIRE(session->free_memory(*allocated).has_value());
}

TEST_CASE("wine-proton also maps and unmaps target memory", "[wine_proton][alloc]")
{
    AllocChild child;
    REQUIRE(child.pid() != 0);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("wine-proton");
    REQUIRE(plugin != nullptr);

    auto session = plugin->open_session(child.pid());
    REQUIRE(session.has_value());
    REQUIRE(session->supports_allocation());

    auto allocated = session->allocate_memory(0x1000, 0);
    REQUIRE(allocated.has_value());
    CHECK((*allocated % 0x1000) == 0);
    REQUIRE(session->free_memory(*allocated).has_value());
}
