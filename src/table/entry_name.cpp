#include "table/entry_name.hpp"

#include <algorithm>
#include <cstddef>
#include <ranges>

namespace slopkit::table
{
    namespace
    {
        // Long enough for any real description, short enough that the ".txt"
        // suffix and a collision number still fit a 255-byte file name.
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

    std::string entry_member_name(std::string_view description, std::span<const std::string> taken)
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

        std::string candidate = stem + ".txt";
        if (!present(candidate))
        {
            return candidate;
        }
        for (std::size_t index = 2;; ++index)
        {
            candidate = stem + std::to_string(index) + ".txt";
            if (!present(candidate))
            {
                return candidate;
            }
        }
    }

} // namespace slopkit::table
