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
    // A shared counter, so the parent can tell the child kept running across a
    // validation: the child's own copy of a private variable would be invisible.
    class ValidateChild
    {
    public:
        ValidateChild()
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

        ~ValidateChild()
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

        ValidateChild(const ValidateChild&)            = delete;
        ValidateChild& operator=(const ValidateChild&) = delete;

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

    // A modest readable region of the child, small enough to probe whole.
    std::optional<slopkit::platform::MappedRegion> small_readable_region(std::uint32_t pid)
    {
        for (const auto& region : slopkit::platform::read_maps(pid))
        {
            const std::uint64_t size = region.end - region.start;
            if (region.readable && size >= 0x1000 && size <= 0x100000)
            {
                return region;
            }
        }
        return std::nullopt;
    }

    void wait_for_progress(const ValidateChild& child)
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

TEST_CASE("linux-proc validates mapped and unmapped ranges through the ABI", "[linux_proc][validate]")
{
    ValidateChild child;
    REQUIRE(child.pid() != 0);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("linux-proc");
    REQUIRE(plugin != nullptr);

    auto session = plugin->open_session(child.pid());
    REQUIRE(session.has_value());
    REQUIRE(session->supports_validation());

    auto region = small_readable_region(child.pid());
    REQUIRE(region.has_value());

    // A mapped, readable range is valid, in whole and in part.
    auto part = session->validate_memory(region->start, 8);
    REQUIRE(part.has_value());
    CHECK(*part);

    auto whole = session->validate_memory(region->start, region->end - region->start);
    REQUIRE(whole.has_value());
    CHECK(*whole);

    // The target keeps running across the validations.
    wait_for_progress(child);

    // An unmapped address is a successful "no", never an error.
    auto zero = session->validate_memory(0, 8);
    REQUIRE(zero.has_value());
    CHECK_FALSE(*zero);

    // A range that starts mapped and overruns the mapping end is invalid.
    const auto maps = slopkit::platform::read_maps(child.pid());
    REQUIRE_FALSE(maps.empty());
    const auto last    = maps.back();
    auto       overrun = session->validate_memory(last.end - 8, 0x1000);
    REQUIRE(overrun.has_value());
    CHECK_FALSE(*overrun);

    // A zero size is an argument error, not a "no".
    auto empty = session->validate_memory(region->start, 0);
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error() == slopkit::process::AccessError::invalid_argument);

    wait_for_progress(child);
}

TEST_CASE("wine-proton also validates ranges", "[wine_proton][validate]")
{
    ValidateChild child;
    REQUIRE(child.pid() != 0);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("wine-proton");
    REQUIRE(plugin != nullptr);

    auto session = plugin->open_session(child.pid());
    REQUIRE(session.has_value());
    REQUIRE(session->supports_validation());

    auto region = small_readable_region(child.pid());
    REQUIRE(region.has_value());

    auto mapped = session->validate_memory(region->start, 8);
    REQUIRE(mapped.has_value());
    CHECK(*mapped);

    auto unmapped = session->validate_memory(0, 8);
    REQUIRE(unmapped.has_value());
    CHECK_FALSE(*unmapped);

    auto empty = session->validate_memory(region->start, 0);
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error() == slopkit::process::AccessError::invalid_argument);
}
