#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace slopkit::table
{

    // How an entry is stored in the archive. The member extension is the
    // persisted kind marker: a value entry is a `.txt` member, a script entry a
    // `.lua` one whose body is the verbatim script.
    enum class EntryKind
    {
        value,
        script,
    };

    // The archive layout: every entry member lives under this prefix and its
    // extension is its kind.
    inline constexpr std::string_view kEntriesPrefix {"entries/"};
    inline constexpr std::string_view kValueMemberExtension {".txt"};
    inline constexpr std::string_view kScriptMemberExtension {".lua"};

    // Derives the archive member name for an entry from its description: a bare
    // file name ending in `extension`. Characters a file name cannot hold (path
    // separators, the Windows-reserved set and non-printables) become '_', the
    // stem is trimmed and length-capped, and a description that is empty or maps
    // to nothing usable falls back to "unnamed". When `taken` already holds the
    // resulting name, the smallest free integer is appended before the extension
    // ("unnamed.txt", "unnamed2.txt", ...); the numbering is per extension, so a
    // value entry and a script entry may share a stem.
    [[nodiscard]] std::string entry_member_name(std::string_view             description,
                                                std::span<const std::string> taken,
                                                std::string_view             extension = kValueMemberExtension);

    // The kind a member name stands for, derived from its extension. A name with
    // neither extension is not a member this format knows.
    [[nodiscard]] std::optional<EntryKind> kind_for_member(std::string_view member_name);

    // The description a member name stands for: the stem inside `entries/`,
    // without the extension.
    [[nodiscard]] std::string member_stem(std::string_view member_name);

} // namespace slopkit::table
