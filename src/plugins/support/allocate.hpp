#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

#include "plugins/support/session.hpp"

namespace slopkit::plugins::support
{
    struct AllocationError
    {
        std::int32_t code {0};
        std::string  message;
    };

    std::expected<std::uint64_t, AllocationError>
    allocate_memory(Session& session, std::uint64_t size, std::uint64_t near_address);

    std::expected<void, AllocationError> free_memory(Session& session, std::uint64_t address);

    [[nodiscard]] bool allocation_in_flight(Session& session);

    void begin_allocation(Session& session);
    void end_allocation(Session& session);

}
