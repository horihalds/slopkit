#pragma once

#include <cstdint>
#include <optional>
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

    // The guest executable of a Wine process: the main PE image Wine maps, named
    // as the guest sees it, with the pointer width its machine implies. A Wine
    // process's own images are the only place that name and that width exist —
    // `/proc/<pid>/exe` is the loader and `/proc/<pid>/comm` is truncated.
    struct GuestImage
    {
        std::string   path;
        std::uint32_t pointer_size {0};
    };

    // Nothing when the process maps no guest executable (a wineserver, a helper)
    // or when its image is of an unhandled machine.
    [[nodiscard]] std::optional<GuestImage>  guest_image(process::ProcessId pid);
    [[nodiscard]] std::optional<std::string> guest_image_name(process::ProcessId pid);

    // The guest image's pointer width, or 0 when there is no guest image to read
    // it from.
    [[nodiscard]] std::uint32_t guest_pointer_size(process::ProcessId pid);

    // The width a PE machine field implies, or nothing for a machine this project
    // does not handle. A Wine loader is a 64-bit host binary even when the guest
    // is 32-bit, so the guest's own header is the only source of its width.
    [[nodiscard]] std::optional<std::uint32_t> pe_pointer_size(std::uint32_t machine);

}
