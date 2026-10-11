#include "plugins/wine_proton/agent_regions.hpp"

#include <algorithm>
#include <ranges>
#include <utility>

#include "plugins/wine_proton/agent/slopkit_agent_protocol.h"

namespace slopkit::plugins::wine_proton
{
    // Whether a Win32 PAGE_* protection permits a plain read; PAGE_NOACCESS and
    // PAGE_EXECUTE do not, and PAGE_GUARD is a one-shot trap.
    bool region_protection_readable(std::uint32_t protection)
    {
        if ((protection & 0x100u) != 0)
        {
            return false;
        }
        switch (protection & 0xFFu)
        {
        case SLOPKIT_AGENT_PAGE_READONLY:
        case SLOPKIT_AGENT_PAGE_READWRITE:
        case SLOPKIT_AGENT_PAGE_WRITECOPY:
        case SLOPKIT_AGENT_PAGE_EXECUTE_READ:
        case SLOPKIT_AGENT_PAGE_EXECUTE_READWRITE:
        case SLOPKIT_AGENT_PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
        }
    }

    namespace
    {
        bool protection_writable(std::uint32_t protection)
        {
            if ((protection & 0x100u) != 0)
            {
                return false;
            }
            switch (protection & 0xFFu)
            {
            case SLOPKIT_AGENT_PAGE_READWRITE:
            case SLOPKIT_AGENT_PAGE_WRITECOPY:
            case SLOPKIT_AGENT_PAGE_EXECUTE_READWRITE:
            case SLOPKIT_AGENT_PAGE_EXECUTE_WRITECOPY:
                return true;
            default:
                return false;
            }
        }

        bool protection_executable(std::uint32_t protection)
        {
            switch (protection & 0xFFu)
            {
            case SLOPKIT_AGENT_PAGE_EXECUTE:
            case SLOPKIT_AGENT_PAGE_EXECUTE_READ:
            case SLOPKIT_AGENT_PAGE_EXECUTE_READWRITE:
            case SLOPKIT_AGENT_PAGE_EXECUTE_WRITECOPY:
                return true;
            default:
                return false;
            }
        }
    } // namespace

    std::vector<platform::MappedRegion> merge_regions(const std::vector<AgentRegionEntry>&       agent_entries,
                                                      const std::vector<platform::MappedRegion>& maps)
    {
        std::vector<platform::MappedRegion> merged;
        merged.reserve(agent_entries.size());
        for (const auto& entry : agent_entries)
        {
            if (entry.state == SLOPKIT_AGENT_MEM_FREE || entry.size == 0)
            {
                continue;
            }

            platform::MappedRegion region;
            region.start      = entry.base;
            region.end        = entry.base + entry.size;
            region.readable   = region_protection_readable(entry.protection);
            region.writable   = protection_writable(entry.protection);
            region.executable = protection_executable(entry.protection);
            region.shared     = entry.type != SLOPKIT_AGENT_MEM_PRIVATE;

            // The procfs entry containing this region's start supplies the file
            // identity; the agent's walk and /proc agree on the committed
            // boundaries, so the start is enough to pick the right line.
            const auto overlay = std::ranges::find_if(maps,
                                                      [&](const platform::MappedRegion& map)
                                                      {
                                                          return map.start <= region.start && region.start < map.end;
                                                      });
            if (overlay != maps.end())
            {
                region.path    = overlay->path;
                region.offset  = overlay->offset;
                region.shared  = overlay->shared;
                region.deleted = overlay->deleted;
            }

            merged.push_back(std::move(region));
        }

        std::ranges::sort(merged, {}, &platform::MappedRegion::start);
        return merged;
    }
} // namespace slopkit::plugins::wine_proton
