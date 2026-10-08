#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace slopkit::table
{

    // One plain-text member of a table archive, in the order it is stored.
    struct ArchiveMember
    {
        std::string name;
        std::string text;
    };

    // Writes `members` to `path` as a stored (uncompressed) ZIP archive. The
    // archive is built beside `path` and renamed over it, so a failed save cannot
    // truncate a good table; the temporary file is removed on every failure.
    [[nodiscard]] std::expected<void, std::string> write_archive(const std::filesystem::path&   path,
                                                                 std::span<const ArchiveMember> members);

    // Reads every member in central-directory order. The error text comes from
    // libzip, so it names the failing step rather than a table member.
    [[nodiscard]] std::expected<std::vector<ArchiveMember>, std::string>
    read_archive(const std::filesystem::path& path);

    // True when `path` begins with the ZIP local-file-header signature. It turns
    // "this is not a slopkit table" into its own message instead of a parse error.
    [[nodiscard]] bool is_zip_archive(const std::filesystem::path& path);

} // namespace slopkit::table
