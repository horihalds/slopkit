#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <utility>
#include <vector>

#include "process/access.hpp"
#include "process/types.hpp"

namespace slopkit::scan
{

    // Reads `size` bytes at `address`; the error mirrors the process seam.
    using ReadFn =
        std::function<std::expected<std::vector<std::byte>, process::AccessError>(std::uint64_t, std::size_t)>;

    // Supplies the regions a scan walks.
    using RegionsFn = std::function<std::vector<process::RegionInfo>()>;

    // The address space a scan walks: a read callback plus the regions to visit.
    struct MemorySource
    {
        ReadFn    read;
        RegionsFn regions;
    };

    // Reads and enumerates through a live session. The session must outlive the
    // source and any scan using it.
    [[nodiscard]] MemorySource make_session_source(process::Session& session);

    // A one-region source over an owned buffer, for tests. `base` is the address
    // of the first buffer byte.
    [[nodiscard]] MemorySource make_buffer_source(std::vector<std::byte> buffer, std::uint64_t base = 0x1000);

} // namespace slopkit::scan
