#pragma once

#include <span>
#include <string>
#include <string_view>

namespace slopkit::table
{

    // Derives the archive member name for an entry from its description: a bare
    // file name ending in ".txt". Characters a file name cannot hold (path
    // separators, the Windows-reserved set and non-printables) become '_', the
    // stem is trimmed and length-capped, and a description that is empty or maps
    // to nothing usable falls back to "unnamed". When `taken` already holds the
    // resulting name, the smallest free integer is appended before ".txt"
    // ("unnamed", "unnamed2", "unnamed3", ...).
    [[nodiscard]] std::string entry_member_name(std::string_view description, std::span<const std::string> taken);

} // namespace slopkit::table
