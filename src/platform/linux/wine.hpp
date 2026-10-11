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

    [[nodiscard]] WineClassification classify_wine_process(const std::vector<std::string>& environ,
                                                           const std::vector<std::string>& cmdline,
                                                           std::string_view                maps_text);

    [[nodiscard]] WineClassification classify_wine_process(process::ProcessId pid);

    [[nodiscard]] std::vector<std::string> read_environ(process::ProcessId pid);
    [[nodiscard]] std::string              read_maps_text(process::ProcessId pid);

    [[nodiscard]] bool is_wine_mapped_path(std::string_view path);

}
