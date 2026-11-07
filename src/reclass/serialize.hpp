#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "reclass/project.hpp"

namespace slopkit::reclass
{
    // The member prefix the .skt archive reserves for the class project; the
    // archive always carries the classes of a saved table, and an archive
    // written before the classes existed carries none of them.
    inline constexpr std::string_view kMemberPrefix = "reclass/";

    // One archive member as plain text, so this module stays free of any
    // dependency on the archive container.
    struct Member
    {
        std::string name;
        std::string text;
    };

    [[nodiscard]] std::vector<Member> to_members(const Project& project);

    [[nodiscard]] std::expected<Project, std::string> from_members(std::span<const Member> members);

}
