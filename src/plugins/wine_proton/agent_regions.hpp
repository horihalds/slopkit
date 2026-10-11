#pragma once

#include <cstdint>
#include <vector>

#include "platform/linux/procfs.hpp"
#include "plugins/wine_proton/agent_client.hpp"

namespace slopkit::plugins::wine_proton
{
    // Whether a Win32 PAGE_* protection permits a plain read: PAGE_NOACCESS and
    // PAGE_EXECUTE do not, and PAGE_GUARD is a one-shot trap.
    [[nodiscard]] bool region_protection_readable(std::uint32_t protection);

    // Merges the agent's VirtualQuery walk with /proc/<pid>/maps: the agent entry
    // supplies the boundaries, commit state and protection, and the overlapping
    // procfs entry supplies path, offset, shared and deleted, so module
    // attribution survives. A range with no procfs counterpart keeps an empty
    // path, offset 0 and `shared` from its mapping type. Free ranges are dropped
    // and the result is sorted by start. Pure, so it is tested without Wine.
    [[nodiscard]] std::vector<platform::MappedRegion> merge_regions(const std::vector<AgentRegionEntry>& agent_entries,
                                                                    const std::vector<platform::MappedRegion>& maps);
} // namespace slopkit::plugins::wine_proton
