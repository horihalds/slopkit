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

    // The profile hook: `nullptr`-free replacement for support::allocate_memory.
    // Its only allocation path is the resident agent, so Wine's own VM manager
    // owns the mapping; injection, handshake or request failures are logged and
    // returned as an error, and the agent is not retried for that session. A
    // Wine mapping that Wine cannot see is never produced.
    class AgentAllocator
    {
    public:
        [[nodiscard]] static std::expected<std::uint64_t, support::AllocationError>
        allocate(support::Session& session, std::uint64_t size, std::uint64_t near_address);

        // The profile hook: releases an address the agent handed out and refuses
        // any other; an address the agent cannot release is an error.
        [[nodiscard]] static std::expected<void, support::AllocationError> free(support::Session& session,
                                                                                std::uint64_t     address);

        // The profile's close hook: SHUTDOWNs the pid's agent (which frees what
        // it allocated) and unmaps the shared region.
        static void close(process::ProcessId pid);

    private:
        AgentAllocator() = delete;
    };

} // namespace slopkit::plugins::wine_proton
