#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "platform/linux/procfs.hpp"
#include "plugins/support/allocate.hpp"
#include "plugins/support/memory.hpp"
#include "plugins/support/session.hpp"
#include "process/types.hpp"

namespace slopkit::plugins::wine_proton
{
    // The profile hooks that reach the resident agent. Allocation is agent-only:
    // Wine's VM manager must own the mapping, so a failure is returned, never
    // patched over. Module enumeration is agent-only too, because only the
    // target's own loader list names the images the way the game does.
    // Introspection (regions, validation, reads, writes) rides the agent while
    // the target can run and falls to the host path while a debug session has it
    // stopped. In both cases injection, handshake or request failures log their
    // reason once, disable the agent for the session and are returned as an
    // error.
    class AgentService
    {
    public:
        // The profile hook: `nullptr`-free replacement for support::allocate_memory.
        // Its only allocation path is the resident agent, so Wine's own VM manager
        // owns the mapping; injection, handshake or request failures are logged and
        // returned as an error, and the agent is not retried for that session. A
        // Wine mapping that Wine cannot see is never produced.
        [[nodiscard]] static std::expected<std::uint64_t, support::AllocationError>
        allocate(support::Session& session, std::uint64_t size, std::uint64_t near_address);

        // The profile hook: releases an address the agent handed out and refuses
        // any other; an address the agent cannot release is an error.
        [[nodiscard]] static std::expected<void, support::AllocationError> free(support::Session& session,
                                                                                std::uint64_t     address);

        // The profile hook: the target's own region list, merged with the
        // procfs paths. The host route answers while the target cannot run.
        [[nodiscard]] static std::expected<std::vector<platform::MappedRegion>, support::AccessFault>
        regions(support::Session& session);

        // The profile hook: the target's own loader list, merged with the procfs
        // paths. Agent-only: while the target cannot run the recorded reason is
        // returned as a fault, never a host-derived module list.
        [[nodiscard]] static std::expected<std::vector<process::ModuleInfo>, support::AccessFault>
        modules(support::Session& session);

        // The profile hook: whether a range is readable, answered by a
        // VirtualQuery in the target while it can run and by the read probe
        // otherwise. A hard agent failure is an error, not a silent false.
        [[nodiscard]] static std::expected<bool, support::AccessFault>
        validate(support::Session& session, std::uint64_t address, std::size_t size);

        // The profile hook: reads through the agent while the target can run and
        // through the host session memory while it cannot. A short transfer is a
        // success; a hard agent failure is an error with its reason.
        [[nodiscard]] static std::expected<support::MemoryResult, support::AccessFault>
        read(support::Session& session, std::uint64_t address, std::span<std::byte> buffer);

        // The profile hook: writes through the agent while the target can run and
        // through the host session memory while it cannot.
        [[nodiscard]] static std::expected<support::MemoryResult, support::AccessFault>
        write(support::Session& session, std::uint64_t address, std::span<const std::byte> data);

        // The profile's close hook: SHUTDOWNs the pid's agent (which frees what
        // it allocated) and unmaps the shared region.
        static void close(process::ProcessId pid);

    private:
        AgentService() = delete;
    };

} // namespace slopkit::plugins::wine_proton
