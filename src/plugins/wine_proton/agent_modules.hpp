#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "platform/linux/procfs.hpp"
#include "plugins/wine_proton/agent_client.hpp"
#include "process/types.hpp"

namespace slopkit::plugins::wine_proton
{
    void decode_utf16(std::span<const char16_t> text, std::string& out);

    [[nodiscard]] std::vector<process::ModuleInfo> merge_modules(std::span<const AgentModuleEntry> image_modules,
                                                                 const std::vector<platform::MappedRegion>& maps);
}
