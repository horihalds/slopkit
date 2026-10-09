#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

#include "plugins/support/session.hpp"

namespace slopkit::plugins::support
{
    // A failed allocation or free, carrying the ABI status plus a human message
    // (the target's -errno text for a refused syscall).
    struct AllocationError
    {
        std::int32_t code {0};
        std::string  message;
    };

    // Maps `size` bytes (page-rounded) of read/write/execute memory inside the
    // session's target and returns the mapping base. `near_address` is a
    // best-effort hint (0 = anywhere). The mapping is recorded on the session so
    // free_memory can unmap exactly it.
    std::expected<std::uint64_t, AllocationError>
    allocate_memory(Session& session, std::uint64_t size, std::uint64_t near_address);

    // Unmaps a mapping allocate_memory of the same session created. An address
    // the session never handed out is SLOPKIT_ERR_NOT_FOUND.
    std::expected<void, AllocationError> free_memory(Session& session, std::uint64_t address);

    // True while this session is running a remote syscall; the debug attach path
    // refuses then, so a user debug session and an allocation never overlap.
    [[nodiscard]] bool allocation_in_flight(Session& session);

    // Sets/clears the in-flight flag. allocate_memory/free_memory use these; the
    // plugin's debug_attach checks allocation_in_flight before seizing.
    void begin_allocation(Session& session);
    void end_allocation(Session& session);

} // namespace slopkit::plugins::support
