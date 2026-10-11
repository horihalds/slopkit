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
    // Decodes UTF-16 code units to UTF-8, substituting U+FFFD for an unpaired
    // surrogate or a truncated pair, so malformed input never throws and never
    // reads past `text`.
    void decode_utf16(std::span<const char16_t> text, std::string& out);

    // Merges the target's own loader list with /proc/<pid>/maps: the loader
    // entry supplies the base, image size, entry point, Wine's base name and
    // the main image, and the overlapping procfs entry supplies path and
    // offset. A module with no procfs counterpart keeps an empty path and
    // offset 0. The result is sorted by base. Pure, so it is tested without
    // Wine.
    [[nodiscard]] std::vector<process::ModuleInfo> merge_modules(std::span<const AgentModuleEntry> image_modules,
                                                                 const std::vector<platform::MappedRegion>& maps);
} // namespace slopkit::plugins::wine_proton
