#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "process/types.hpp"

namespace slopkit::plugins::wine_proton
{

    // Everything the Linux side needs to reach one Wine/Proton target with the
    // resident agent: the wine binary and the environment the target itself
    // runs under (Proton's own wine and WINEPREFIX, never the system ones), the
    // guest PE's pointer size, and the target image as the Windows path the
    // injector matches on.
    struct WineTarget
    {
        process::ProcessId       pid {0};
        std::filesystem::path    exe_path;
        std::filesystem::path    wine_binary;
        std::uint32_t            pointer_size {0};
        std::vector<std::string> environment;
        std::string              image_path;
    };

    // Derives the target description from /proc/<pid>. A failure names what
    // could not be found, so the plugin can report it.
    [[nodiscard]] std::expected<WineTarget, std::string> derive_wine_target(process::ProcessId pid);

    // The Z: form of an absolute Linux path, the way Wine's default drive
    // mapping presents the host filesystem to the target.
    [[nodiscard]] std::string linux_path_to_windows(const std::string& path);

} // namespace slopkit::plugins::wine_proton
