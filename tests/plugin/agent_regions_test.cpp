#include <catch2/catch.hpp>

#include <cstdint>
#include <vector>

#include "platform/linux/procfs.hpp"
#include "plugins/wine_proton/agent/slopkit_agent_protocol.h"
#include "plugins/wine_proton/agent_client.hpp"
#include "plugins/wine_proton/agent_regions.hpp"

namespace
{
    using slopkit::platform::MappedRegion;
    using slopkit::plugins::wine_proton::AgentRegionEntry;
    using slopkit::plugins::wine_proton::merge_regions;

    AgentRegionEntry committed(std::uint64_t base, std::uint64_t size, std::uint32_t protection, std::uint32_t type)
    {
        AgentRegionEntry entry;
        entry.base       = base;
        entry.size       = size;
        entry.state      = SLOPKIT_AGENT_MEM_COMMIT;
        entry.protection = protection;
        entry.type       = type;
        return entry;
    }
} // namespace

TEST_CASE("the region merge takes boundaries from the agent and paths from procfs", "[wine_proton][agent]")
{
    const std::vector<AgentRegionEntry> entries {
        committed(0x1000, 0x1000, SLOPKIT_AGENT_PAGE_READONLY, SLOPKIT_AGENT_MEM_IMAGE),
        committed(0x2000, 0x1000, SLOPKIT_AGENT_PAGE_EXECUTE_READWRITE, SLOPKIT_AGENT_MEM_PRIVATE),
    };

    MappedRegion overlay;
    overlay.start   = 0x1000;
    overlay.end     = 0x2000;
    overlay.offset  = 0x40;
    overlay.shared  = true;
    overlay.deleted = true;
    overlay.path    = "/usr/lib/wine/x86_64-windows/ntdll.dll";

    const auto merged = merge_regions(entries, {overlay});
    REQUIRE(merged.size() == 2);

    // The overlapping procfs line supplies the file identity; the agent supplies
    // the boundaries and the protection.
    CHECK(merged[0].start == 0x1000);
    CHECK(merged[0].end == 0x2000);
    CHECK(merged[0].path == "/usr/lib/wine/x86_64-windows/ntdll.dll");
    CHECK(merged[0].offset == 0x40);
    CHECK(merged[0].shared);
    CHECK(merged[0].deleted);
    CHECK(merged[0].readable);
    CHECK_FALSE(merged[0].writable);
    CHECK_FALSE(merged[0].executable);

    // A range with no procfs counterpart keeps an empty path and offset 0, and
    // takes `shared` from its mapping type.
    CHECK(merged[1].path.empty());
    CHECK(merged[1].offset == 0);
    CHECK_FALSE(merged[1].shared);
    CHECK(merged[1].readable);
    CHECK(merged[1].writable);
    CHECK(merged[1].executable);
}

TEST_CASE("the region merge sorts by start, drops free ranges and keeps mapped as shared", "[wine_proton][agent]")
{
    AgentRegionEntry free_range = committed(0x3000, 0x1000, SLOPKIT_AGENT_PAGE_NOACCESS, SLOPKIT_AGENT_MEM_PRIVATE);
    free_range.state            = SLOPKIT_AGENT_MEM_FREE;

    const std::vector<AgentRegionEntry> entries {
        committed(0x9000, 0x1000, SLOPKIT_AGENT_PAGE_READWRITE, SLOPKIT_AGENT_MEM_MAPPED),
        free_range,
        committed(0x1000, 0x1000, SLOPKIT_AGENT_PAGE_NOACCESS, SLOPKIT_AGENT_MEM_PRIVATE),
    };

    const auto merged = merge_regions(entries, {});
    REQUIRE(merged.size() == 2);
    CHECK(merged[0].start == 0x1000);
    CHECK(merged[1].start == 0x9000);

    // PAGE_NOACCESS is not readable, and an agent-only mapped range is shared.
    CHECK_FALSE(merged[0].readable);
    CHECK(merged[1].shared);
}
