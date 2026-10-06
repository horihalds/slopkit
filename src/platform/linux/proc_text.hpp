#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "process/types.hpp"

namespace slopkit::platform
{

    // Text helpers shared by the /proc, Wine and desktop-entry readers. They live
    // outside the anonymous namespaces of their callers so they can be unit
    // tested on their own.

    // `value` without its leading and trailing whitespace.
    [[nodiscard]] std::string_view trim(std::string_view value);

    // The lines of `text`, without their newline. A trailing newline yields a
    // final empty line; an empty string yields one empty line.
    [[nodiscard]] std::vector<std::string_view> split_lines(std::string_view text);

    // The whole file at `path`, or nullopt when it cannot be opened.
    [[nodiscard]] std::optional<std::string> read_file(const std::filesystem::path& path);

    // The path of one entry under a process's /proc directory.
    [[nodiscard]] std::string proc_entry(process::ProcessId pid, std::string_view name);

    // True when `value` is non-empty and holds only ASCII digits.
    [[nodiscard]] bool is_all_digits(std::string_view value);

    // The lower-cased final path component, splitting both POSIX and Windows
    // separators so `C:\windows\winedevice.exe` reduces to `winedevice.exe`.
    [[nodiscard]] std::string basename_lower(std::string_view path);

} // namespace slopkit::platform
