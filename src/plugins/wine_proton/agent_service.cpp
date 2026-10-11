#include "plugins/wine_proton/agent_service.hpp"

#include <chrono>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "plugin/plugin_api.h"
#include "plugins/support/allocate.hpp"
#include "plugins/support/validate.hpp"
#include "plugins/wine_proton/agent_client.hpp"
#include "plugins/wine_proton/agent_modules.hpp"
#include "plugins/wine_proton/agent_regions.hpp"
#include "plugins/wine_proton/wine_target.hpp"

namespace slopkit::plugins::wine_proton
{
    namespace
    {
        // The injector starts a fresh wine process, which can be slow on a cold
        // prefix; the request timeout applies to every exchange after that.
        constexpr std::chrono::milliseconds kLaunchTimeout {20000};
        constexpr std::chrono::milliseconds kRequestTimeout {std::chrono::milliseconds {kAgentRequestTimeoutMs}};
        constexpr std::chrono::milliseconds kShutdownTimeout {200};

        // One target's agent. Once `disabled_reason` is set the agent path is
        // never retried for that process: every later call fails with that
        // reason, so a hardened or unsupported target pays the cost once.
        struct AgentState
        {
            std::optional<AgentClient> client;
            std::string                disabled_reason;
        };

        // Allocations are one-shot operations and the shared region serves one
        // request at a time, so a single lock for every tracked process is
        // enough and keeps the state trivially consistent.
        std::recursive_mutex                               g_mutex;
        std::unordered_map<process::ProcessId, AgentState> g_agents;

        void disable(AgentState& state, process::ProcessId pid, const std::string& reason)
        {
            state.client.reset();
            state.disabled_reason = reason;
            log::warning(log::category::process,
                         std::format("wine-proton agent for pid {} is off for this session ({}); the agent path is "
                                     "unavailable",
                                     pid,
                                     reason));
        }

        // Brings the agent up for `pid`, or records why it never will be.
        // Returns a pointer that stays valid while g_mutex is held.
        AgentClient* ensure_agent(AgentState& state, process::ProcessId pid)
        {
            if (state.client)
            {
                return &*state.client;
            }
            if (!state.disabled_reason.empty())
            {
                return nullptr;
            }

            const auto target = derive_wine_target(pid);
            if (!target)
            {
                disable(state, pid, target.error());
                return nullptr;
            }
            auto launched = AgentClient::launch(*target, kLaunchTimeout);
            if (!launched)
            {
                disable(state, pid, launched.error());
                return nullptr;
            }

            log::info(log::category::process,
                      std::format("wine-proton resident agent is serving pid {}: wine pid {}, {}-bit, base {:#x}, "
                                  "injected by {}",
                                  pid,
                                  launched->windows_pid(),
                                  target->pointer_size * 8,
                                  launched->agent_base(),
                                  target->wine_binary.string()));
            state.client = std::move(*launched);
            return &*state.client;
        }

        // Runs a call on the agent with the session marked as in flight, so a
        // debug attach refuses while the target is being touched.
        template<typename Call>
        auto with_agent(support::Session& session, const Call& call)
        {
            support::begin_allocation(session);
            auto result = call();
            support::end_allocation(session);
            return result;
        }

        // Whether the target can currently run the agent's service thread: a
        // debug session stops the whole target, and our own remote syscall
        // suspends it for the moment, so the host path answers instead.
        bool target_can_run(support::Session& session)
        {
            return !session.debug.attached() && !support::allocation_in_flight(session);
        }
    } // namespace

    std::expected<std::uint64_t, support::AllocationError>
    AgentService::allocate(support::Session& session, std::uint64_t size, std::uint64_t near_address)
    {
        const std::lock_guard lock(g_mutex);
        if (session.debug.attached())
        {
            return std::unexpected(
                support::AllocationError {SLOPKIT_ERR_PERMISSION_DENIED, "the target is being debugged"});
        }

        AgentState&  state = g_agents[session.pid];
        AgentClient* agent = ensure_agent(state, session.pid);
        if (agent == nullptr)
        {
            return std::unexpected(support::AllocationError {SLOPKIT_ERR_IO, state.disabled_reason});
        }

        const auto placed = with_agent(session,
                                       [&]
                                       {
                                           return near_address != 0
                                                    ? agent->allocate_near(size, near_address, kRequestTimeout)
                                                    : agent->allocate(size, 0, kRequestTimeout);
                                       });
        if (!placed)
        {
            const std::string reason = placed.error();
            disable(state, session.pid, reason);
            return std::unexpected(support::AllocationError {SLOPKIT_ERR_IO, reason});
        }

        log::debug(
            log::category::process,
            std::format(
                "allocated {} bytes at {:#x} for pid {} through the resident agent", size, *placed, session.pid));
        return *placed;
    }

    std::expected<void, support::AllocationError> AgentService::free(support::Session& session, std::uint64_t address)
    {
        const std::lock_guard lock(g_mutex);
        if (session.debug.attached())
        {
            return std::unexpected(
                support::AllocationError {SLOPKIT_ERR_PERMISSION_DENIED, "the target is being debugged"});
        }

        const auto entry = g_agents.find(session.pid);
        if (entry == g_agents.end() || !entry->second.client || !entry->second.client->owns(address))
        {
            return std::unexpected(support::AllocationError {SLOPKIT_ERR_NOT_FOUND, "not this session's allocation"});
        }

        AgentState& state = entry->second;
        const auto  freed = with_agent(session,
                                       [&]
                                       {
                                          return state.client->free(address, kRequestTimeout);
                                       });
        if (!freed)
        {
            // The agent handed this address out but could not release it. Give
            // up on the agent for the session: SHUTDOWN frees everything it
            // allocated, and every later call fails instead of falling back.
            const std::string reason = freed.error();
            disable(state, session.pid, reason);
            return std::unexpected(support::AllocationError {SLOPKIT_ERR_IO, reason});
        }

        log::debug(log::category::process,
                   std::format("released {:#x} for pid {} through the resident agent", address, session.pid));
        return {};
    }

    std::expected<std::vector<platform::MappedRegion>, support::AccessFault>
    AgentService::regions(support::Session& session)
    {
        const std::lock_guard lock(g_mutex);
        if (!target_can_run(session))
        {
            return platform::read_maps(session.pid);
        }

        AgentState&  state = g_agents[session.pid];
        AgentClient* agent = ensure_agent(state, session.pid);
        if (agent == nullptr)
        {
            return std::unexpected(support::AccessFault {SLOPKIT_ERR_IO, state.disabled_reason});
        }

        std::vector<AgentRegionEntry> entries;
        std::uint64_t                 cursor = 0;
        for (;;)
        {
            const auto batch = with_agent(session,
                                          [&]
                                          {
                                              return agent->enumerate(cursor, 0, kRequestTimeout);
                                          });
            if (!batch)
            {
                const std::string reason = batch.error();
                disable(state, session.pid, reason);
                return std::unexpected(support::AccessFault {SLOPKIT_ERR_IO, reason});
            }
            entries.insert(entries.end(), batch->regions.begin(), batch->regions.end());
            cursor = batch->next_cursor;
            if (cursor == 0)
            {
                break;
            }
        }

        return merge_regions(entries, platform::read_maps(session.pid));
    }

    std::expected<std::vector<process::ModuleInfo>, support::AccessFault>
    AgentService::modules(support::Session& session)
    {
        const std::lock_guard lock(g_mutex);
        // Agent-only: a module list that silently switched to the host's view
        // would be the very discrepancy this path removes, so the reason the
        // agent cannot serve is returned instead.
        if (session.debug.attached())
        {
            return std::unexpected(
                support::AccessFault {SLOPKIT_ERR_PERMISSION_DENIED, "the target is being debugged"});
        }
        if (support::allocation_in_flight(session))
        {
            return std::unexpected(support::AccessFault {SLOPKIT_ERR_IO, "a call is in flight on this session"});
        }

        AgentState&  state = g_agents[session.pid];
        AgentClient* agent = ensure_agent(state, session.pid);
        if (agent == nullptr)
        {
            return std::unexpected(support::AccessFault {SLOPKIT_ERR_IO, state.disabled_reason});
        }

        std::vector<AgentModuleEntry> entries;
        std::uint64_t                 cursor = 0;
        for (;;)
        {
            const auto batch = with_agent(session,
                                          [&]
                                          {
                                              return agent->modules(cursor, 0, kRequestTimeout);
                                          });
            if (!batch)
            {
                const std::string reason = batch.error();
                disable(state, session.pid, reason);
                return std::unexpected(support::AccessFault {SLOPKIT_ERR_IO, reason});
            }
            entries.insert(entries.end(), batch->modules.begin(), batch->modules.end());
            cursor = batch->next_index;
            if (cursor == 0)
            {
                break;
            }
        }

        return merge_modules(entries, platform::read_maps(session.pid));
    }

    std::expected<bool, support::AccessFault>
    AgentService::validate(support::Session& session, std::uint64_t address, std::size_t size)
    {
        const std::lock_guard lock(g_mutex);
        if (!target_can_run(session))
        {
            return support::range_is_readable(session, address, size);
        }

        AgentState&  state = g_agents[session.pid];
        AgentClient* agent = ensure_agent(state, session.pid);
        if (agent == nullptr)
        {
            return std::unexpected(support::AccessFault {SLOPKIT_ERR_IO, state.disabled_reason});
        }

        const auto info = with_agent(session,
                                     [&]
                                     {
                                         return agent->query(address, kRequestTimeout);
                                     });
        if (!info)
        {
            const std::string reason = info.error();
            disable(state, session.pid, reason);
            return std::unexpected(support::AccessFault {SLOPKIT_ERR_IO, reason});
        }
        if (info->state != SLOPKIT_AGENT_MEM_COMMIT || !region_protection_readable(info->protection))
        {
            return false;
        }
        // The whole range must sit inside the single region QUERY reported.
        return address + size <= info->base + info->size;
    }

    std::expected<support::MemoryResult, support::AccessFault>
    AgentService::read(support::Session& session, std::uint64_t address, std::span<std::byte> buffer)
    {
        const std::lock_guard lock(g_mutex);
        if (!target_can_run(session))
        {
            const auto host = session.mem.read(address, buffer);
            if (!host)
            {
                return std::unexpected(
                    support::AccessFault {support::status_for(host.error()), support::message_for(host.error())});
            }
            return support::MemoryResult {host->bytes, support::method_flag(host->primitive)};
        }

        AgentState&  state = g_agents[session.pid];
        AgentClient* agent = ensure_agent(state, session.pid);
        if (agent == nullptr)
        {
            return std::unexpected(support::AccessFault {SLOPKIT_ERR_IO, state.disabled_reason});
        }

        const auto moved = with_agent(session,
                                      [&]
                                      {
                                          return agent->read(address, buffer, kRequestTimeout);
                                      });
        if (!moved)
        {
            const std::string reason = moved.error();
            disable(state, session.pid, reason);
            return std::unexpected(support::AccessFault {SLOPKIT_ERR_IO, reason});
        }
        return support::MemoryResult {*moved, SLOPKIT_ACCESS_AGENT};
    }

    std::expected<support::MemoryResult, support::AccessFault>
    AgentService::write(support::Session& session, std::uint64_t address, std::span<const std::byte> data)
    {
        const std::lock_guard lock(g_mutex);
        if (!target_can_run(session))
        {
            const auto host = session.mem.write(address, data);
            if (!host)
            {
                return std::unexpected(
                    support::AccessFault {support::status_for(host.error()), support::message_for(host.error())});
            }
            return support::MemoryResult {host->bytes, support::method_flag(host->primitive)};
        }

        AgentState&  state = g_agents[session.pid];
        AgentClient* agent = ensure_agent(state, session.pid);
        if (agent == nullptr)
        {
            return std::unexpected(support::AccessFault {SLOPKIT_ERR_IO, state.disabled_reason});
        }

        const auto moved = with_agent(session,
                                      [&]
                                      {
                                          return agent->write(address, data, kRequestTimeout);
                                      });
        if (!moved)
        {
            const std::string reason = moved.error();
            disable(state, session.pid, reason);
            return std::unexpected(support::AccessFault {SLOPKIT_ERR_IO, reason});
        }
        return support::MemoryResult {*moved, SLOPKIT_ACCESS_AGENT};
    }

    void AgentService::close(process::ProcessId pid)
    {
        const std::lock_guard lock(g_mutex);
        const auto            entry = g_agents.find(pid);
        if (entry == g_agents.end())
        {
            return;
        }
        if (entry->second.client)
        {
            // SHUTDOWN frees what the agent allocated and unmaps the region; the
            // per-session directory goes with the transport.
            entry->second.client->shutdown(kShutdownTimeout);
        }
        g_agents.erase(entry);
    }

} // namespace slopkit::plugins::wine_proton
