#include "reclass/dissect.hpp"

#include <cstring>

namespace slopkit::reclass
{
    namespace
    {
        constexpr double kLargestPlausibleValue = 999999.0;

        template<typename Value>
        Value read_at(std::span<const std::byte> bytes)
        {
            Value value {};
            std::memcpy(&value, bytes.data(), sizeof(value));
            return value;
        }

        bool plausible(double value)
        {
            return value > -kLargestPlausibleValue && value < kLargestPlausibleValue;
        }
    }

    std::optional<NodeKind> guess_kind(const DissectInput& input, const DissectLookup& lookup)
    {
        if (!is_hex_kind(input.current) || input.offset % 4 != 0)
        {
            return std::nullopt;
        }
        if (input.bytes.size() < kind_size(input.current))
        {
            return std::nullopt;
        }

        if (input.bytes.size() >= 8)
        {
            const std::uint64_t pointer = read_at<std::uint64_t>(input.bytes);
            if (pointer != 0 && lookup.readable && lookup.readable(pointer, 1))
            {
                return NodeKind::pointer;
            }
        }

        if (input.bytes.size() >= 4)
        {
            const float as_float = read_at<float>(input.bytes);
            if (as_float != 0.0F && plausible(as_float))
            {
                return NodeKind::float32;
            }

            const std::int32_t as_int = read_at<std::int32_t>(input.bytes);
            if (as_int != 0 && plausible(as_int))
            {
                return NodeKind::int32;
            }
        }

        if (input.bytes.size() >= 8 && input.offset % 8 == 0)
        {
            const double as_double = read_at<double>(input.bytes);
            if (as_double != 0.0 && plausible(as_double))
            {
                return NodeKind::float64;
            }
        }
        return std::nullopt;
    }

}
