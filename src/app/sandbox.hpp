#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace slopkit::app
{
    [[nodiscard]] std::optional<std::filesystem::path> sandbox_binary_path(const std::filesystem::path& directory = {});

    [[nodiscard]] std::expected<std::int64_t, std::string> launch_sandbox(const std::filesystem::path& directory = {});

}
