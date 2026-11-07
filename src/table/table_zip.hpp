#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace slopkit::table
{
    struct ArchiveMember
    {
        std::string name;
        std::string text;
    };

    [[nodiscard]] std::expected<void, std::string> write_archive(const std::filesystem::path&   path,
                                                                 std::span<const ArchiveMember> members);

    [[nodiscard]] std::expected<std::vector<ArchiveMember>, std::string>
    read_archive(const std::filesystem::path& path);

    [[nodiscard]] bool is_zip_archive(const std::filesystem::path& path);

}
