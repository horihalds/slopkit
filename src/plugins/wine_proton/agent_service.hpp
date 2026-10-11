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
    // the target can run and falls to the host path while the target cannot run
    // the agent.
    //
    // Failure is retryable: an exchange that fails records its reason and, once
    // every slot is retired, drops the client so a later call may bring a fresh
    // agent up (bounded by kMaxAgentRevives with a cooldown). A stopped target is
    // not a failure: our own SIGSTOP pause answers reads and writes from the host
    // path, `modules` from the last agent-derived list recorded for the session,
    // and `allocate`/`free` with a retryable busy error, without disabling the
    // agent.
    class AgentService
    {
    public:
        // The profile hook: `nullptr`-free replacement for support::allocate_memory.
        // Its only allocation path is the resident agent, so Wine's own VM manager
        // owns the mapping; a failure is logged and returned as a retryable error,
        // and a paused target answers busy instead of disabling the agent.
        [[nodiscard]] static std::expected<std::uint64_t, support::AllocationError>
        allocate(support::Session& session, std::uint64_t size, std::uint64_t near_address);

        // The profile hook: releases an address the agent handed out and refuses
        // any other; an address the agent cannot release is a retryable error.
        [[nodiscard]] static std::expected<void, support::AllocationError> free(support::Session& session,
                                                                                std::uint64_t     address);

        // The profile hook: the target's own region list, merged with the
        // procfs paths. The host's /proc/maps view answers while the target
        // cannot run the agent.
        [[nodiscard]] static std::expected<std::vector<platform::MappedRegion>, support::AccessFault>
        regions(support::Session& session);

        // The profile hook: the target's own loader list, merged with the procfs
        // paths. While the target cannot run the agent the last agent-derived
        // list recorded for the session answers, never a host-derived one.
        [[nodiscard]] static std::expected<std::vector<process::ModuleInfo>, support::AccessFault>
        modules(support::Session& session);

        // The profile hook: whether a range is readable, answered by a
        // VirtualQuery in the target while it can run and by the read probe
        // otherwise. A failed exchange falls back to the probe, not a false.
        [[nodiscard]] static std::expected<bool, support::AccessFault>
        validate(support::Session& session, std::uint64_t address, std::size_t size);

        // The profile hook: reads through the agent while the target can run and
        // through the host session memory while it cannot or when the exchange
        // fails. A short transfer is a success.
        [[nodiscard]] static std::expected<support::MemoryResult, support::AccessFault>
        read(support::Session& session, std::uint64_t address, std::span<std::byte> buffer);

        // The profile hook: writes through the agent while the target can run and
        // through the host session memory while it cannot or when the exchange
        // fails.
        [[nodiscard]] static std::expected<support::MemoryResult, support::AccessFault>
        write(support::Session& session, std::uint64_t address, std::span<const std::byte> data);

        // The profile's close hook: waits for in-flight requests, then SHUTDOWNs
        // the pid's agent (which frees what it allocated) and unmaps the shared
        // region.
        static void close(process::ProcessId pid);

        // The profile's `debug_excluded_threads` hook: the threads of the pid's
        // resident agent (its dispatcher and every worker it has started), so a
        // debug attach leaves them running and they keep serving. Empty when the
        // agent is not mapped or its threads cannot be recognised; the attach
        // then proceeds unexempted, exactly as it did before.
        [[nodiscard]] static std::vector<process::ProcessId> agent_threads(support::Session& session);

    private:
        AgentService() = delete;
    };

} // namespace slopkit::plugins::wine_proton
