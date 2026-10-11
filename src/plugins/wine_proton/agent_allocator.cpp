#include "plugins/wine_proton/agent_allocator.hpp"

#include <chrono>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "plugins/support/allocate.hpp"
#include "plugins/wine_proton/agent_client.hpp"
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
        // never retried for that process: every later call goes to ptrace, so a
        // hardened or unsupported target pays the injection cost once.
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
                         std::format("wine-proton agent for pid {} is off for this session ({}); allocation falls "
                                     "back to ptrace",
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

        // Runs a call on the agent with the session marked as allocating, so a
        // debug attach refuses while the target is being touched.
        template<typename Call>
        auto with_agent(support::Session& session, const Call& call)
        {
            support::begin_allocation(session);
            auto result = call();
            support::end_allocation(session);
            return result;
        }
    } // namespace

    std::expected<std::uint64_t, support::AllocationError>
    AgentAllocator::allocate(support::Session& session, std::uint64_t size, std::uint64_t near_address)
    {
        {
            const std::lock_guard lock(g_mutex);
            if (!session.debug.attached())
            {
                AgentState& state = g_agents[session.pid];
                if (AgentClient* agent = ensure_agent(state, session.pid))
                {
                    const auto placed =
                        with_agent(session,
                                   [&]
                                   {
                                       return near_address != 0
                                                ? agent->allocate_near(size, near_address, kRequestTimeout)
                                                : agent->allocate(size, 0, kRequestTimeout);
                                   });
                    if (placed)
                    {
                        log::debug(log::category::process,
                                   std::format("allocated {} bytes at {:#x} for pid {} through the resident agent",
                                               size,
                                               *placed,
                                               session.pid));
                        return *placed;
                    }
                    disable(state, session.pid, placed.error());
                }
            }
        }

        const auto placed = support::allocate_memory(session, size, near_address);
        if (placed)
        {
            log::debug(
                log::category::process,
                std::format("allocated {} bytes at {:#x} for pid {} through ptrace", size, *placed, session.pid));
        }
        return placed;
    }

    std::expected<void, support::AllocationError> AgentAllocator::free(support::Session& session, std::uint64_t address)
    {
        {
            const std::lock_guard lock(g_mutex);
            const auto            entry = g_agents.find(session.pid);
            if (entry != g_agents.end() && entry->second.client && entry->second.client->owns(address)
                && !session.debug.attached())
            {
                AgentState& state = entry->second;
                const auto  freed = with_agent(session,
                                               [&]
                                               {
                                                  return state.client->free(address, kRequestTimeout);
                                               });
                if (freed)
                {
                    log::debug(
                        log::category::process,
                        std::format("released {:#x} for pid {} through the resident agent", address, session.pid));
                    return {};
                }

                // The agent handed this address out but could not release it.
                // Record its size so the ptrace path can still unmap it, then
                // give up on the agent for the rest of the session.
                if (const auto size = state.client->allocation_size(address))
                {
                    support::record_allocation(session, address, *size);
                }
                disable(state, session.pid, freed.error());
            }
        }
        return support::free_memory(session, address);
    }

    void AgentAllocator::close(process::ProcessId pid)
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
