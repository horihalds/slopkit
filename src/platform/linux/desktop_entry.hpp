#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slopkit::platform
{

    // The subset of a [Desktop Entry] group the process list needs.
    struct DesktopEntry
    {
        std::string name;
        std::string exec;
        std::string type;
        bool        no_display {false};
        bool        hidden {false};
    };

    // Parses the [Desktop Entry] group of a .desktop file. Returns nullopt when
    // the text has no such group.
    [[nodiscard]] std::optional<DesktopEntry> parse_desktop_entry(std::string_view text);

    // The executable basename an Exec value refers to; arguments and field
    // codes are stripped and the directory removed.
    [[nodiscard]] std::optional<std::string> desktop_exec_basename(std::string_view exec);

    // Directories searched for .desktop files: $XDG_DATA_HOME (or
    // ~/.local/share) plus $XDG_DATA_DIRS (or /usr/local/share:/usr/share),
    // each with `applications` appended.
    [[nodiscard]] std::vector<std::filesystem::path> default_application_dirs();

    // Basenames of the executables referenced by the visible Application
    // entries under `application_dirs`, sorted and de-duplicated.
    [[nodiscard]] std::vector<std::string>
    scan_desktop_executables(std::span<const std::filesystem::path> application_dirs);

    // True when the basename of `exe_path` appears in `executables`.
    [[nodiscard]] bool is_desktop_application(std::string_view exe_path, std::span<const std::string> executables);

} // namespace slopkit::platform
