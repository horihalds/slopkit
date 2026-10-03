#include "scan/matcher.hpp"

#include <algorithm>
#include <cstring>
#include <variant>

#include "scan/engine.hpp"

namespace slopkit::scan
{

    Matcher Matcher::build(const ScanConfig& config)
    {
        Matcher matcher;
        matcher.scan_type_   = config.type;
        matcher.value_type_  = config.value_type;
        matcher.value_       = config.value;
        matcher.value_upper_ = config.value_upper;
        matcher.hex_         = config.hex;
        matcher.window_      = effective_size(config.value_type, config.value);

        if (config.type == ScanType::unknown_initial_value)
        {
            matcher.kind_ = Kind::all;
            return matcher;
        }

        if (config.type == ScanType::exact_value)
        {
            switch (config.value_type)
            {
            case ValueType::byte:
            case ValueType::int16:
            case ValueType::int32:
            case ValueType::int64:
            case ValueType::all:
                if (const auto* integer = std::get_if<std::int64_t>(&config.value))
                {
                    matcher.kind_           = Kind::exact_integer;
                    matcher.integer_needle_ = static_cast<std::uint64_t>(*integer);
                    matcher.needle_         = encode_value(config.value_type, config.value);
                }
                break;
            case ValueType::float32:
            case ValueType::float64:
                if (const auto* real = std::get_if<double>(&config.value))
                {
                    matcher.kind_        = Kind::exact_real;
                    matcher.real_needle_ = *real;
                }
                break;
            case ValueType::string:
                if (std::holds_alternative<std::string>(config.value))
                {
                    matcher.kind_   = Kind::exact_bytes;
                    matcher.needle_ = encode_value(config.value_type, config.value);
                }
                break;
            case ValueType::byte_array:
                if (std::holds_alternative<std::vector<std::byte>>(config.value))
                {
                    matcher.kind_   = Kind::exact_bytes;
                    matcher.needle_ = encode_value(config.value_type, config.value);
                }
                break;
            }
        }

        if (matcher.kind_ == Kind::exact_integer || matcher.kind_ == Kind::exact_bytes)
        {
            // Pick the first needle byte that is neither 0x00 nor 0xFF: a
            // memchr for such a byte skips most candidates. A needle that is
            // uniformly 0x00/0xFF (e.g. the value 0 or 0xFFFFFFFF) gets no
            // prefilter, since every byte would be a candidate anyway.
            for (std::size_t index = 0; index < matcher.needle_.size(); ++index)
            {
                const std::byte byte = matcher.needle_[index];
                if (byte != std::byte {0x00} && byte != std::byte {0xFF})
                {
                    matcher.prefilter_        = true;
                    matcher.prefilter_offset_ = index;
                    matcher.prefilter_byte_   = byte;
                    break;
                }
            }
        }

        return matcher;
    }

    std::size_t Matcher::window() const noexcept
    {
        return window_;
    }

    bool Matcher::matches_all() const noexcept
    {
        return kind_ == Kind::all;
    }

    bool Matcher::match_integer(std::span<const std::byte> candidate) const noexcept
    {
        switch (value_type_)
        {
        case ValueType::byte:
        {
            std::int8_t value {};
            std::memcpy(&value, candidate.data(), sizeof(value));
            return value == static_cast<std::int8_t>(integer_needle_);
        }
        case ValueType::int16:
        {
            std::int16_t value {};
            std::memcpy(&value, candidate.data(), sizeof(value));
            return value == static_cast<std::int16_t>(integer_needle_);
        }
        case ValueType::int32:
        case ValueType::all:
        {
            std::int32_t value {};
            std::memcpy(&value, candidate.data(), sizeof(value));
            return value == static_cast<std::int32_t>(integer_needle_);
        }
        case ValueType::int64:
        {
            std::int64_t value {};
            std::memcpy(&value, candidate.data(), sizeof(value));
            return value == static_cast<std::int64_t>(integer_needle_);
        }
        default:
            return false;
        }
    }

    bool Matcher::match(std::span<const std::byte> candidate) const noexcept
    {
        switch (kind_)
        {
        case Kind::all:
            return true;
        case Kind::generic:
            return matches(scan_type_, value_type_, candidate, value_, value_upper_, hex_);
        case Kind::exact_real:
        {
            if (candidate.size() < value_size(value_type_))
            {
                return false;
            }
            double current = 0.0;
            if (value_type_ == ValueType::float32)
            {
                float real {};
                std::memcpy(&real, candidate.data(), sizeof(real));
                current = real;
            }
            else
            {
                std::memcpy(&current, candidate.data(), sizeof(current));
            }
            return current == real_needle_;
        }
        case Kind::exact_bytes:
        {
            if (candidate.size() < needle_.size())
            {
                return false;
            }
            return std::memcmp(candidate.data(), needle_.data(), needle_.size()) == 0;
        }
        case Kind::exact_integer:
            if (candidate.size() < value_size(value_type_))
            {
                return false;
            }
            return match_integer(candidate);
        }
        return false;
    }

    std::size_t Matcher::find_prefiltered(std::span<const std::byte> bytes,
                                          std::size_t                offset,
                                          std::size_t                limit,
                                          std::size_t                alignment) const noexcept
    {
        const auto* const base           = reinterpret_cast<const unsigned char*>(bytes.data());
        const int         wanted         = static_cast<int>(std::to_integer<unsigned char>(prefilter_byte_));
        const std::size_t last_candidate = std::min(bytes.size() - window_, limit - 1);
        const std::size_t search_begin   = offset + prefilter_offset_;
        const std::size_t search_end     = last_candidate + prefilter_offset_;
        if (search_begin > search_end)
        {
            return limit;
        }

        std::size_t position = search_begin;
        while (position <= search_end)
        {
            const void* const found = std::memchr(base + position, wanted, search_end - position + 1);
            if (found == nullptr)
            {
                return limit;
            }
            position = static_cast<std::size_t>(static_cast<const unsigned char*>(found) - base);

            const std::size_t candidate = position - prefilter_offset_;
            if ((candidate - offset) % alignment == 0 && match(bytes.subspan(candidate, window_)))
            {
                return candidate;
            }
            ++position;
        }
        return limit;
    }

    std::size_t Matcher::find(std::span<const std::byte> bytes,
                              std::size_t                offset,
                              std::size_t                limit,
                              std::size_t                alignment) const noexcept
    {
        if (alignment == 0)
        {
            alignment = 1;
        }
        if (window_ == 0 || bytes.size() < window_)
        {
            return limit;
        }
        if (limit > bytes.size())
        {
            limit = bytes.size();
        }
        if (offset >= limit || offset > bytes.size() - window_)
        {
            return limit;
        }
        if (kind_ == Kind::all)
        {
            return offset;
        }
        if (prefilter_)
        {
            return find_prefiltered(bytes, offset, limit, alignment);
        }

        for (std::size_t candidate = offset; candidate < limit && candidate + window_ <= bytes.size();
             candidate += alignment)
        {
            if (match(bytes.subspan(candidate, window_)))
            {
                return candidate;
            }
        }
        return limit;
    }

} // namespace slopkit::scan
