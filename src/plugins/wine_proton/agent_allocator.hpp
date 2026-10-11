#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>

#include "plugins/support/allocate.hpp"
#include "plugins/support/session.hpp"
#include "process/types.hpp"

namespace slopkit::plugins::wine_proton
{

    // The resident-agent side of the wine-proton allocator: it tries the in
    // target agent first (so Wine's own VM manager owns the mapping) and, on
    // any failure, permanently falls back to the shared ptrace allocator for
    // the rest of the session. Which path served a call is always logged.
    class AgentAllocator
    {
    public:
        // The profile hook: `nullptr`-free replacement for support::allocate_memory.
        [[nodiscard]] static std::expected<std::uint64_t, support::AllocationError>
        allocate(support::Session& session, std::uint64_t size, std::uint64_t near_address);

        // The profile hook: releases an address the agent handed out through the
        // agent, and any other address through the shared path.
        [[nodiscard]] static std::expected<void, support::AllocationError> free(support::Session& session,
                                                                                std::uint64_t     address);

        // The profile's close hook: SHUTDOWNs the pid's agent (which frees what
        // it allocated) and unmaps the shared region.
        static void close(process::ProcessId pid);

    private:
        AgentAllocator() = delete;
    };

} // namespace slopkit::plugins::wine_proton
