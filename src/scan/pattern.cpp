#include "scan/pattern.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <iterator>

namespace slopkit::scan
{
    namespace
    {
        bool is_space(char value) noexcept
        {
            return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f' || value == '\v';
        }

        // Value of a hex digit, or -1 when `value` is not one.
        int hex_digit(char value) noexcept
        {
            if (value >= '0' && value <= '9')
            {
                return value - '0';
            }
            if (value >= 'a' && value <= 'f')
            {
                return value - 'a' + 10;
            }
            if (value >= 'A' && value <= 'F')
            {
                return value - 'A' + 10;
            }
            return -1;
        }

        std::string not_a_byte(std::string_view token)
        {
            return std::format("'{}' is not a byte (use two hex digits or '?')", token);
        }
    } // namespace

    std::expected<BytePattern, std::string> BytePattern::parse(std::string_view text)
    {
        BytePattern pattern;
        std::size_t index = 0;
        while (index < text.size())
        {
            if (is_space(text[index]))
            {
                ++index;
                continue;
            }
            if (text[index] == '?')
            {
                std::size_t end = index;
                while (end < text.size() && text[end] == '?')
                {
                    ++end;
                }
                if (end - index > 2)
                {
                    return std::unexpected(not_a_byte(text.substr(index, end - index)));
                }
                pattern.needle_.push_back(std::byte {0x00});
                pattern.mask_.push_back(false);
                index = end;
                continue;
            }
            // A concrete byte is exactly two hex digits.
            if (index + 2 > text.size())
            {
                return std::unexpected(not_a_byte(text.substr(index, text.size() - index)));
            }
            const int high = hex_digit(text[index]);
            const int low  = hex_digit(text[index + 1]);
            if (high < 0 || low < 0)
            {
                return std::unexpected(not_a_byte(text.substr(index, 2)));
            }
            pattern.needle_.push_back(static_cast<std::byte>((high << 4) | low));
            pattern.mask_.push_back(true);
            index += 2;
        }

        if (pattern.needle_.empty())
        {
            return std::unexpected(std::string {"the pattern is empty"});
        }

        const auto anchor = std::find(pattern.mask_.begin(), pattern.mask_.end(), true);
        if (anchor == pattern.mask_.end())
        {
            return std::unexpected(std::string {"the pattern has no concrete byte"});
        }
        pattern.anchor_ = static_cast<std::size_t>(std::distance(pattern.mask_.begin(), anchor));
        return pattern;
    }

    std::size_t BytePattern::size() const noexcept
    {
        return needle_.size();
    }

    std::optional<std::size_t> BytePattern::find(std::span<const std::byte> bytes, std::size_t from) const
    {
        if (needle_.empty() || bytes.size() < needle_.size())
        {
            return std::nullopt;
        }
        const std::size_t last = bytes.size() - needle_.size();
        for (std::size_t base = from; base <= last; ++base)
        {
            // Locate the anchor byte before verifying the rest of the frame, so
            // a long wildcard-heavy pattern does not compare every candidate.
            if (bytes[base + anchor_] != needle_[anchor_])
            {
                continue;
            }
            bool matched = true;
            for (std::size_t offset = 0; offset < needle_.size(); ++offset)
            {
                if (mask_[offset] && bytes[base + offset] != needle_[offset])
                {
                    matched = false;
                    break;
                }
            }
            if (matched)
            {
                return base;
            }
        }
        return std::nullopt;
    }

} // namespace slopkit::scan
