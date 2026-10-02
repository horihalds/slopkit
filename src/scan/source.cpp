#include "scan/source.hpp"

#include <algorithm>
#include <memory>

namespace slopkit::scan
{

    MemorySource make_session_source(process::Session& session)
    {
        MemorySource source;
        source.read = [&session](std::uint64_t address, std::size_t size)
        {
            return session.read(address, size);
        };
        source.regions = [&session]()
        {
            auto regions = session.regions();
            return regions ? std::move(*regions) : std::vector<process::RegionInfo> {};
        };
        return source;
    }

    MemorySource make_buffer_source(std::vector<std::byte> buffer, std::uint64_t base)
    {
        auto shared = std::make_shared<std::vector<std::byte>>(std::move(buffer));

        MemorySource source;
        source.read = [shared, base](std::uint64_t address,
                                     std::size_t   size) -> std::expected<std::vector<std::byte>, process::AccessError>
        {
            if (address < base)
            {
                return std::unexpected(process::AccessError::not_found);
            }
            const std::uint64_t offset = address - base;
            if (offset > shared->size())
            {
                return std::unexpected(process::AccessError::not_found);
            }
            const auto        first     = shared->begin() + static_cast<std::ptrdiff_t>(offset);
            const std::size_t available = shared->size() - static_cast<std::size_t>(offset);
            const std::size_t take      = std::min(size, available);
            return std::vector<std::byte>(first, first + static_cast<std::ptrdiff_t>(take));
        };
        source.regions = [shared, base]()
        {
            process::RegionInfo region;
            region.start    = base;
            region.end      = base + shared->size();
            region.readable = true;
            region.writable = true;
            return std::vector<process::RegionInfo> {std::move(region)};
        };
        return source;
    }

} // namespace slopkit::scan
