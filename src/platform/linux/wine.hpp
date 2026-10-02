#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "process/types.hpp"

namespace slopkit::platform
{

    enum class WineFlavor
    {
        none,
        wine,
        proton,
    };

    // Which inputs carried evidence and what that evidence was.
    struct WineEvidence
    {
        bool                     environ_signal {false};
        bool                     cmdline_signal {false};
        bool                     maps_signal {false};
        std::vector<std::string> details;
    };

    struct WineClassification
    {
        WineFlavor   flavor {WineFlavor::none};
        WineEvidence evidence;

        [[nodiscard]] bool claimed() const noexcept
        {
            return flavor != WineFlavor::none;
        }
    };

    // Classifies from raw process data. Any one signal is enough to claim the
    // process, so detection survives processes where some signals are missing.
    [[nodiscard]] WineClassification classify_wine_process(const std::vector<std::string>& environ,
                                                           const std::vector<std::string>& cmdline,
                                                           std::string_view                maps_text);

    // Reads the live process and classifies it.
    [[nodiscard]] WineClassification classify_wine_process(process::ProcessId pid);

    [[nodiscard]] std::vector<std::string> read_environ(process::ProcessId pid);
    [[nodiscard]] std::string              read_maps_text(process::ProcessId pid);

    // True when a mapped path belongs to a Wine/Proton install.
    [[nodiscard]] bool is_wine_mapped_path(std::string_view path);

} // namespace slopkit::platform
