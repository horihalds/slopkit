#include "reclass/nodes/number.hpp"

#include <bit>
#include <cstdint>
#include <format>

namespace slopkit::reclass
{
    std::string_view BitFieldNode::extra_text() const noexcept
    {
        extra_cache_ = std::format("{} bit(s)", bits);
        return extra_cache_;
    }

    std::uint64_t BitFieldNode::read(std::span<const std::byte> bytes) const noexcept
    {
        const std::size_t width = (bits + 7) / 8;
        if (bits == 0 || bytes.size() < width)
        {
            return 0;
        }

        std::uint64_t value = 0;
        for (std::size_t index = 0; index < width; ++index)
        {
            value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[index])) << (index * 8);
        }

        const std::uint64_t mask = bits >= 64 ? ~std::uint64_t {0} : ((std::uint64_t {1} << bits) - 1);
        value &= mask;
        if (signed_bits && bits < 64 && (value & (std::uint64_t {1} << (bits - 1))) != 0)
        {
            value |= ~mask;
        }
        return value;
    }

}
