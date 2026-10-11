#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "process/types.hpp"

namespace slopkit::plugins::wine_proton
{
    struct WineTarget
    {
        process::ProcessId       pid {0};
        std::filesystem::path    exe_path;
        std::filesystem::path    wine_binary;
        std::uint32_t            pointer_size {0};
        std::vector<std::string> environment;
        std::string              image_path;
    };

    [[nodiscard]] std::expected<WineTarget, std::string> derive_wine_target(process::ProcessId pid);

    [[nodiscard]] std::vector<std::string> injector_environment(const std::vector<std::string>& environment);

    [[nodiscard]] std::string linux_path_to_windows(const std::string& path);

    [[nodiscard]] bool target_can_reach(process::ProcessId pid, const std::filesystem::path& path);

    [[nodiscard]] std::expected<std::filesystem::path, std::string> make_staging_directory(process::ProcessId pid);

}
