#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slopkit::platform
{
    struct DesktopEntry
    {
        std::string name;
        std::string exec;
        std::string type;
        bool        no_display {false};
        bool        hidden {false};
    };

    [[nodiscard]] std::optional<DesktopEntry> parse_desktop_entry(std::string_view text);

    [[nodiscard]] std::optional<std::string> desktop_exec_basename(std::string_view exec);

    [[nodiscard]] std::vector<std::filesystem::path> default_application_dirs();

    [[nodiscard]] std::vector<std::string>
    scan_desktop_executables(std::span<const std::filesystem::path> application_dirs);

    [[nodiscard]] bool is_desktop_application(std::string_view exe_path, std::span<const std::string> executables);

}
