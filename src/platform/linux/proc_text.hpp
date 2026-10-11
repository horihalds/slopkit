#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "process/types.hpp"

namespace slopkit::platform
{
    [[nodiscard]] std::string_view trim(std::string_view value);

    [[nodiscard]] std::vector<std::string_view> split_lines(std::string_view text);

    [[nodiscard]] std::optional<std::string> read_file(const std::filesystem::path& path);

    [[nodiscard]] std::string proc_entry(process::ProcessId pid, std::string_view name);

    [[nodiscard]] bool is_all_digits(std::string_view value);

    [[nodiscard]] std::string basename_lower(std::string_view path);

}
