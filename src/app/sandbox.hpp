#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace slopkit::app
{

    // `<directory>/slopkit-sandbox`, or nullopt when it is not a regular file.
    // `directory` defaults to executable_directory(), the way the plugin search
    // path resolves its own sibling directory; tests inject a scratch directory.
    [[nodiscard]] std::optional<std::filesystem::path> sandbox_binary_path(const std::filesystem::path& directory = {});

    // Starts the sandbox as slopkit's own child in its own session, so the kernel
    // permits reading it while it still outlives slopkit. Returns the new pid, or
    // a human-readable reason when the binary is missing or the spawn fails.
    [[nodiscard]] std::expected<std::int64_t, std::string> launch_sandbox(const std::filesystem::path& directory = {});

} // namespace slopkit::app
