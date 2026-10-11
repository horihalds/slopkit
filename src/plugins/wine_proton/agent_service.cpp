#include "plugins/wine_proton/agent_service.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "plugin/plugin_api.h"
#include "plugins/support/allocate.hpp"
#include "plugins/wine_proton/agent_client.hpp"
#include "plugins/wine_proton/agent_modules.hpp"
#include "plugins/wine_proton/agent_regions.hpp"
#include "plugins/wine_proton/wine_target.hpp"

namespace slopkit::plugins::wine_proton
{
    namespace
    {
        constexpr std::chrono::milliseconds kLaunchTimeout {20000};
        constexpr std::chrono::milliseconds kRequestTimeout {std::chrono::milliseconds {kAgentRequestTimeoutMs}};
        constexpr std::chrono::milliseconds kShutdownTimeout {200};
        constexpr unsigned                  kMaxAgentRevives = 3;
        constexpr std::chrono::seconds      kReviveCooldown {5};

        struct AgentState
        {
            std::shared_ptr<AgentClient>          client;
            unsigned                              in_flight {0};
            unsigned                              revives {0};
            std::chrono::steady_clock::time_point cooldown_until {};
            std::string                           reason;
            bool                                  permanent {false};
            bool                                  bringing_up {false};
            bool                                  has_modules {false};
            std::vector<process::ModuleInfo>      modules;
            std::condition_variable               idle;
        };

        std::mutex                                                              g_mutex;
        std::unordered_map<process::ProcessId, std::shared_ptr<AgentState>>     g_agents;
        std::unordered_map<process::ProcessId, std::vector<process::ProcessId>> g_agent_threads;

        class CallScope
        {
        public:
            explicit CallScope(std::shared_ptr<AgentState> state) : state_(std::move(state)) {}

            CallScope(const CallScope&)            = delete;
            CallScope& operator=(const CallScope&) = delete;

            CallScope(CallScope&& other) noexcept : state_(std::move(other.state_)) {}

            CallScope& operator=(CallScope&&) = delete;

            ~CallScope()
            {
                const std::lock_guard lock(g_mutex);
                if (--state_->in_flight == 0)
                {
                    state_->idle.notify_all();
                }
            }

            [[nodiscard]] AgentState& state() const noexcept
            {
                return *state_;
            }

        private:
            std::shared_ptr<AgentState> state_;
        };

        std::unique_ptr<CallScope> begin_call(process::ProcessId pid)
        {
            const std::lock_guard lock(g_mutex);
            auto&                 slot = g_agents[pid];
            if (!slot)
            {
                slot = std::make_shared<AgentState>();
            }
            ++slot->in_flight;
            return std::make_unique<CallScope>(slot);
        }

        std::string failure_reason(const AgentState& state)
        {
            const std::lock_guard lock(g_mutex);
            return state.reason.empty() ? std::string("the resident agent is not available") : state.reason;
        }

        std::shared_ptr<AgentClient> ensure_client(AgentState& state, process::ProcessId pid)
        {
            {
                const std::lock_guard lock(g_mutex);
                if (state.client)
                {
                    return state.client;
                }
                if (state.permanent || state.revives >= kMaxAgentRevives)
                {
                    state.permanent = true;
                    return nullptr;
                }
                if (state.bringing_up || std::chrono::steady_clock::now() < state.cooldown_until)
                {
                    return nullptr;
                }
                state.bringing_up = true;
            }

            std::shared_ptr<AgentClient> fresh;
            std::string                  failure;
            bool                         permanent = false;
            const auto                   target    = derive_wine_target(pid);
            if (!target)
            {
                failure   = target.error();
                permanent = true;
            }
            else
            {
                auto launched = AgentClient::launch(*target, kLaunchTimeout);
                if (!launched)
                {
                    failure = launched.error();
                }
                else
                {
                    log::info(log::category::process,
                              std::format("wine-proton resident agent is serving pid {}: wine pid {}, {}-bit, base "
                                          "{:#x}, injected by {}",
                                          pid,
                                          launched->windows_pid(),
                                          target->pointer_size * 8,
                                          launched->agent_base(),
                                          target->wine_binary.string()));
                    fresh = std::make_shared<AgentClient>(std::move(*launched));
                }
            }

            const std::lock_guard lock(g_mutex);
            state.bringing_up = false;
            if (fresh)
            {
                state.client = fresh;
                state.reason.clear();
                return fresh;
            }
            ++state.revives;
            state.reason         = failure;
            state.cooldown_until = std::chrono::steady_clock::now() + kReviveCooldown;
            if (permanent || state.revives >= kMaxAgentRevives)
            {
                state.permanent = true;
            }
            log::warning(log::category::process,
                         std::format("wine-proton agent for pid {} could not start ({}); {}",
                                     pid,
                                     failure,
                                     state.permanent ? "the agent path is unavailable for this session"
                                                     : "a later call will try again"));
            return nullptr;
        }

        void note_failure(const std::shared_ptr<AgentClient>& client,
                          AgentState&                         state,
                          process::ProcessId                  pid,
                          const std::string&                  reason)
        {
            const bool exhausted = client->slots_exhausted();
            bool       dropped   = false;
            {
                const std::lock_guard lock(g_mutex);
                state.reason = reason;
                if (exhausted && state.client == client)
                {
                    state.client.reset();
                    ++state.revives;
                    state.cooldown_until = std::chrono::steady_clock::now() + kReviveCooldown;
                    if (state.revives >= kMaxAgentRevives)
                    {
                        state.permanent = true;
                    }
                    dropped = true;
                }
            }
            log::warning(log::category::process,
                         std::format("wine-proton agent request for pid {} failed ({}); {}",
                                     pid,
                                     reason,
                                     dropped ? "every slot is retired, a later call will try a fresh agent"
                                             : "the remaining slots still serve"));
        }

        std::optional<std::vector<process::ModuleInfo>> cached_modules(const AgentState& state)
        {
            const std::lock_guard lock(g_mutex);
            if (!state.has_modules)
            {
                return std::nullopt;
            }
            return state.modules;
        }

        void store_modules(AgentState& state, std::vector<process::ModuleInfo> modules)
        {
            const std::lock_guard lock(g_mutex);
            state.modules     = std::move(modules);
            state.has_modules = true;
        }

        std::optional<std::uint64_t> thread_stack_start(process::ProcessId pid, process::ProcessId tid)
        {
            std::ifstream stat(std::format("/proc/{}/task/{}/stat", pid, tid));
            if (!stat)
            {
                return std::nullopt;
            }
            std::string line;
            if (!std::getline(stat, line))
            {
                return std::nullopt;
            }
            const auto close = line.rfind(')');
            if (close == std::string::npos)
            {
                return std::nullopt;
            }
            std::istringstream fields(line.substr(close + 1));
            std::string        token;
            int                field = 2;
            while (fields >> token)
            {
                ++field;
                if (field == 28)
                {
                    return std::strtoull(token.c_str(), nullptr, 10);
                }
            }
            return std::nullopt;
        }

        bool thread_holds_magic(support::Session& session, std::uint64_t startstack)
        {
            constexpr std::size_t  kChunk = 64 * 1024;
            const std::size_t      window = SLOPKIT_AGENT_STACK_SCAN_BYTES;
            const std::uint64_t    bottom = startstack > window ? startstack - window : 0;
            std::vector<std::byte> buffer(kChunk);
            std::uint64_t          address = startstack;
            while (address > bottom)
            {
                const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, address - bottom));
                address -= want;
                const auto read = session.mem.read(address, std::span<std::byte>(buffer.data(), want));
                if (!read)
                {
                    continue;
                }
                for (std::size_t offset = 0; offset + sizeof(std::uint64_t) <= read->bytes; ++offset)
                {
                    std::uint64_t value = 0;
                    std::memcpy(&value, buffer.data() + offset, sizeof(value));
                    if (value == SLOPKIT_AGENT_THREAD_MAGIC)
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        std::vector<process::ProcessId> scan_agent_threads(support::Session& session, process::ProcessId pid)
        {
            const auto                      threads = platform::read_threads(pid);
            std::vector<process::ProcessId> found;
            for (const auto& thread : threads)
            {
                if (thread.name == SLOPKIT_AGENT_THREAD_NAME)
                {
                    found.push_back(thread.tid);
                }
            }
            if (!found.empty())
            {
                return found;
            }
            for (const auto& thread : threads)
            {
                const auto startstack = thread_stack_start(pid, thread.tid);
                if (startstack && thread_holds_magic(session, *startstack))
                {
                    found.push_back(thread.tid);
                }
            }
            return found;
        }
    }

    std::expected<std::uint64_t, support::AllocationError>
    AgentService::allocate(support::Session& session, std::uint64_t size, std::uint64_t near_address)
    {
        const auto  call  = begin_call(session.pid);
        AgentState& state = call->state();
        if (session.suspended)
        {
            return std::unexpected(
                support::AllocationError {SLOPKIT_ERR_IO, "the target is paused; try again once it is running"});
        }

        const auto client = ensure_client(state, session.pid);
        if (!client)
        {
            return std::unexpected(support::AllocationError {SLOPKIT_ERR_IO, failure_reason(state)});
        }

        const auto placed = near_address != 0 ? client->allocate_near(size, near_address, kRequestTimeout)
                                              : client->allocate(size, 0, kRequestTimeout);
        if (!placed)
        {
            note_failure(client, state, session.pid, placed.error());
            return std::unexpected(support::AllocationError {SLOPKIT_ERR_IO, placed.error()});
        }

        log::debug(
            log::category::process,
            std::format(
                "allocated {} bytes at {:#x} for pid {} through the resident agent", size, *placed, session.pid));
        return *placed;
    }

    std::expected<void, support::AllocationError> AgentService::free(support::Session& session, std::uint64_t address)
    {
        const auto  call  = begin_call(session.pid);
        AgentState& state = call->state();
        if (session.suspended)
        {
            return std::unexpected(
                support::AllocationError {SLOPKIT_ERR_IO, "the target is paused; try again once it is running"});
        }

        const auto client = ensure_client(state, session.pid);
        if (!client)
        {
            return std::unexpected(support::AllocationError {SLOPKIT_ERR_IO, failure_reason(state)});
        }
        if (!client->owns(address))
        {
            return std::unexpected(support::AllocationError {SLOPKIT_ERR_NOT_FOUND, "not this session's allocation"});
        }

        const auto freed = client->free(address, kRequestTimeout);
        if (!freed)
        {
            note_failure(client, state, session.pid, freed.error());
            return std::unexpected(support::AllocationError {SLOPKIT_ERR_IO, freed.error()});
        }

        log::debug(log::category::process,
                   std::format("released {:#x} for pid {} through the resident agent", address, session.pid));
        return {};
    }

    std::expected<std::vector<platform::MappedRegion>, support::AccessFault>
    AgentService::regions(support::Session& session)
    {
        const auto  call  = begin_call(session.pid);
        AgentState& state = call->state();
        if (session.suspended)
        {
            return platform::read_maps(session.pid);
        }

        const auto client = ensure_client(state, session.pid);
        if (!client)
        {
            return platform::read_maps(session.pid);
        }

        std::vector<AgentRegionEntry> entries;
        std::uint64_t                 cursor = 0;
        for (;;)
        {
            const auto batch = client->enumerate(cursor, 0, kRequestTimeout);
            if (!batch)
            {
                note_failure(client, state, session.pid, batch.error());
                return platform::read_maps(session.pid);
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
        const auto  call  = begin_call(session.pid);
        AgentState& state = call->state();
        if (session.suspended)
        {
            if (const auto cached = cached_modules(state))
            {
                return *cached;
            }
            return std::unexpected(
                support::AccessFault {SLOPKIT_ERR_IO, "the target is paused and no module list has been recorded yet"});
        }

        const auto client = ensure_client(state, session.pid);
        if (!client)
        {
            if (session.debug.attached())
            {
                if (const auto cached = cached_modules(state))
                {
                    return *cached;
                }
            }
            return std::unexpected(support::AccessFault {SLOPKIT_ERR_IO, failure_reason(state)});
        }

        std::vector<AgentModuleEntry> entries;
        std::uint64_t                 cursor = 0;
        for (;;)
        {
            const auto batch = client->modules(cursor, 0, kRequestTimeout);
            if (!batch)
            {
                note_failure(client, state, session.pid, batch.error());
                if (session.debug.attached())
                {
                    if (const auto cached = cached_modules(state))
                    {
                        return *cached;
                    }
                }
                return std::unexpected(support::AccessFault {SLOPKIT_ERR_IO, batch.error()});
            }
            entries.insert(entries.end(), batch->modules.begin(), batch->modules.end());
            cursor = batch->next_index;
            if (cursor == 0)
            {
                break;
            }
        }

        auto merged = merge_modules(entries, platform::read_maps(session.pid));
        store_modules(state, merged);
        return merged;
    }

    void AgentService::close(process::ProcessId pid)
    {
        std::shared_ptr<AgentState> state;
        {
            const std::lock_guard lock(g_mutex);
            const auto            entry = g_agents.find(pid);
            if (entry == g_agents.end())
            {
                return;
            }
            state            = entry->second;
            state->permanent = true;
        }

        std::shared_ptr<AgentClient> client;
        {
            std::unique_lock lock(g_mutex);
            state->idle.wait(lock,
                             [&]
                             {
                                 return state->in_flight == 0;
                             });
            client = std::move(state->client);
            g_agents.erase(pid);
            g_agent_threads.erase(pid);
        }

        if (client)
        {
            client->shutdown(kShutdownTimeout);
        }
    }

    std::vector<process::ProcessId> AgentService::agent_threads(support::Session& session)
    {
        auto found = scan_agent_threads(session, session.pid);
        {
            const std::lock_guard lock(g_mutex);
            g_agent_threads[session.pid] = found;
        }
        if (found.empty())
        {
            log::warning(log::category::process,
                         std::format("could not recognise the resident agent's threads for pid {}; a debug attach "
                                     "will stop them too",
                                     session.pid));
        }
        return found;
    }

}
