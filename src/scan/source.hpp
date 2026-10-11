#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <utility>
#include <vector>

#include "process/access.hpp"
#include "process/types.hpp"

namespace slopkit::scan
{
    using ReadFn =
        std::function<std::expected<std::vector<std::byte>, process::AccessError>(std::uint64_t, std::size_t)>;

    using ReadIntoFn =
        std::function<std::expected<std::size_t, process::AccessError>(std::uint64_t, std::span<std::byte>)>;

    using RegionsFn = std::function<std::vector<process::RegionInfo>()>;

    struct MemorySource
    {
        ReadFn     read;
        ReadIntoFn read_into;
        RegionsFn  regions;
    };

    // Reads and enumerates through a live session. The session must outlive the
    // source and any scan using it.
    [[nodiscard]] MemorySource make_session_source(process::Session& session);

    [[nodiscard]] MemorySource make_buffer_source(std::vector<std::byte> buffer, std::uint64_t base = 0x1000);

}
