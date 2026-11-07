#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace slopkit::app
{
    inline constexpr std::string_view kSandboxProcessName = "slopkit-sandbox";

    [[nodiscard]] std::optional<std::filesystem::path> sandbox_binary_path(const std::filesystem::path& directory = {});

    [[nodiscard]] std::expected<std::int64_t, std::string> launch_sandbox(const std::filesystem::path& directory = {});

    // True when the identifier names the sandbox: the process name itself or an
    // executable path's basename, compared case-insensitively.
    [[nodiscard]] bool is_sandbox_identifier(std::string_view identifier) noexcept;

    // SIGTERM, a bounded wait for the direct child, then SIGKILL; an already gone
    // pid is a success.
    [[nodiscard]] std::expected<void, std::string> terminate_sandbox(std::int64_t pid);

}
