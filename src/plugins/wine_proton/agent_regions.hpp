#pragma once

#include <cstdint>
#include <vector>

#include "platform/linux/procfs.hpp"
#include "plugins/wine_proton/agent_client.hpp"

namespace slopkit::plugins::wine_proton
{
    [[nodiscard]] bool region_protection_readable(std::uint32_t protection);

    [[nodiscard]] std::vector<platform::MappedRegion> merge_regions(const std::vector<AgentRegionEntry>& agent_entries,
                                                                    const std::vector<platform::MappedRegion>& maps);
}
