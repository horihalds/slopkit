#include "plugins/support/validate.hpp"

#include <algorithm>
#include <span>
#include <vector>

namespace slopkit::plugins::support
{
    bool range_is_readable(Session& session, std::uint64_t address, std::size_t size)
    {
        constexpr std::size_t  kChunkMax {64 * 1024};
        const std::size_t      chunk = std::min(size, kChunkMax);
        std::vector<std::byte> buffer(chunk);

        std::size_t offset = 0;
        while (offset < size)
        {
            const std::size_t wanted   = std::min(chunk, size - offset);
            const auto        transfer = session.mem.read(address + offset, std::span {buffer.data(), wanted});
            if (!transfer || transfer->bytes < wanted)
            {
                return false;
            }
            offset += wanted;
        }
        return true;
    }

} // namespace slopkit::plugins::support
