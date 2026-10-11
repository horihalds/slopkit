#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace slopkit::table
{
    enum class EntryKind
    {
        value,
        script,
    };

    inline constexpr std::string_view kEntriesPrefix {"entries/"};
    inline constexpr std::string_view kValueMemberExtension {".txt"};
    inline constexpr std::string_view kScriptMemberExtension {".lua"};

    [[nodiscard]] std::string entry_member_name(std::string_view             description,
                                                std::span<const std::string> taken,
                                                std::string_view             extension = kValueMemberExtension);

    [[nodiscard]] std::optional<EntryKind> kind_for_member(std::string_view member_name);

    [[nodiscard]] std::string member_stem(std::string_view member_name);

}
