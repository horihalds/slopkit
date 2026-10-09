#include "table/entry_name.hpp"

#include <algorithm>
#include <cstddef>
#include <ranges>

namespace slopkit::table
{
    namespace
    {
        // Long enough for any real description, short enough that the extension
        // and a collision number still fit a 255-byte file name.
        constexpr std::size_t kMaxStemBytes = 96;

        bool is_hostile(unsigned char character)
        {
            if (character < 0x20 || character == 0x7F)
            {
                return true;
            }
            switch (character)
            {
            case '/':
            case '\\':
            case ':':
            case '*':
            case '?':
            case '"':
            case '<':
            case '>':
            case '|':
                return true;
            default:
                return false;
            }
        }

        std::string_view trim_whitespace(std::string_view value)
        {
            const auto first = value.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos)
            {
                return {};
            }
            const auto last = value.find_last_not_of(" \t\r\n");
            return value.substr(first, last - first + 1);
        }
    } // namespace

    std::string
    entry_member_name(std::string_view description, std::span<const std::string> taken, std::string_view extension)
    {
        std::string stem;
        stem.reserve(description.size());
        for (const char character : trim_whitespace(description))
        {
            stem.push_back(is_hostile(static_cast<unsigned char>(character)) ? '_' : character);
        }

        // A trailing dot or space is dropped by some file systems, so the stem
        // must not end in one.
        while (!stem.empty() && (stem.back() == '.' || stem.back() == ' '))
        {
            stem.pop_back();
        }

        if (stem.size() > kMaxStemBytes)
        {
            stem.resize(kMaxStemBytes);
            // Do not cut a multi-byte UTF-8 sequence in half.
            while (!stem.empty() && (static_cast<unsigned char>(stem.back()) & 0xC0U) == 0x80U)
            {
                stem.pop_back();
            }
        }

        if (stem.empty() || stem == "." || stem == "..")
        {
            stem = "unnamed";
        }

        const auto present = [taken](const std::string& candidate)
        {
            return std::ranges::find(taken, candidate) != taken.end();
        };

        std::string candidate = stem + std::string(extension);
        if (!present(candidate))
        {
            return candidate;
        }
        for (std::size_t index = 2;; ++index)
        {
            candidate = stem + std::to_string(index) + std::string(extension);
            if (!present(candidate))
            {
                return candidate;
            }
        }
    }

    std::optional<EntryKind> kind_for_member(std::string_view member_name)
    {
        if (member_name.ends_with(kScriptMemberExtension))
        {
            return EntryKind::script;
        }
        if (member_name.ends_with(kValueMemberExtension))
        {
            return EntryKind::value;
        }
        return std::nullopt;
    }

    std::string member_stem(std::string_view member_name)
    {
        if (member_name.starts_with(kEntriesPrefix))
        {
            member_name.remove_prefix(kEntriesPrefix.size());
        }
        for (const std::string_view extension : {kValueMemberExtension, kScriptMemberExtension})
        {
            if (member_name.ends_with(extension))
            {
                member_name.remove_suffix(extension.size());
                break;
            }
        }
        return std::string(member_name);
    }

} // namespace slopkit::table
