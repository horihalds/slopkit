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
        // A transient failure must not kill the agent for the session, but the
        // target may pay the injection cost only this many times, and never
        // twice within the cooldown.
        constexpr unsigned                  kMaxAgentRevives = 3;
        constexpr std::chrono::seconds      kReviveCooldown {5};

        // One target's agent plus the policy that decides whether a later call
        // may bring a fresh one up. Every field is guarded by g_mutex; the
        // AgentClient itself is thread-safe once a shared_ptr copies it out.
        struct AgentState
        {
            std::shared_ptr<AgentClient>          client;
            unsigned                              in_flight {0};
            unsigned                              revives {0};
            std::chrono::steady_clock::time_point cooldown_until {};
            std::string                           reason;
            bool                                  permanent {false};
            bool                                  bringing_up {false};
            // The last agent-derived module list, so `modules` can answer while
            // our own pause has the target (and the agent's threads) stopped and
            // the list cannot have changed.
            bool                                  has_modules {false};
            std::vector<process::ModuleInfo>      modules;
            std::condition_variable               idle;
        };

        std::mutex                                                              g_mutex;
        std::unordered_map<process::ProcessId, std::shared_ptr<AgentState>>     g_agents;
        // The threads the last identification found for each pid's agent, so a
        // debug attach can name them; cleared with the session.
        std::unordered_map<process::ProcessId, std::vector<process::ProcessId>> g_agent_threads;

        // A call that keeps its target's agent in flight, so close() never
        // unmaps a region another thread is still transacting on. The counter
        // is set under g_mutex; the exchange itself runs outside it.
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

        // Brings a fresh agent up for `pid`, honouring the revive policy, and
        // returns it; nullptr means "use the host path or answer busy", with the
        // reason recorded on the state. The launch runs without g_mutex and
        // `bringing_up` keeps two threads from injecting at once.
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
                // Not a Wine process at all: no retry can ever help.
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

        // Records a failed exchange. Only once every advertised slot has been
        // retired is the client dropped and a bounded revive scheduled; a single
        // failed slot leaves the rest of the pool serving.
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

        // The host path for a read: process_vm/procfs, the same primitive the
        // linux-proc plugin uses.
        std::expected<support::MemoryResult, support::AccessFault>
        host_read(support::Session& session, std::uint64_t address, std::span<std::byte> buffer)
        {
            const auto host = session.mem.read(address, buffer);
            if (!host)
            {
                return std::unexpected(
                    support::AccessFault {support::status_for(host.error()), support::message_for(host.error())});
            }
            return support::MemoryResult {host->bytes, support::method_flag(host->primitive)};
        }

        std::expected<support::MemoryResult, support::AccessFault>
        host_write(support::Session& session, std::uint64_t address, std::span<const std::byte> data)
        {
            const auto host = session.mem.write(address, data);
            if (!host)
            {
                return std::unexpected(
                    support::AccessFault {support::status_for(host.error()), support::message_for(host.error())});
            }
            return support::MemoryResult {host->bytes, support::method_flag(host->primitive)};
        }

        // Field 28 of /proc/<pid>/task/<tid>/stat is startstack: the top of the
        // thread's stack, below which its live frames sit.
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
            // comm is field 2 and may itself hold spaces and parentheses, so
            // count from the last ')' on: the first token there is field 3.
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

        // Scans the stack window below `startstack` for the live marker every
        // agent loop keeps. The read goes through the host memory path, so it
        // needs no ptrace window and cannot collide with an allocation.
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
                    // Unmapped inside the window: step over it and keep walking.
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

        // The agent's threads: the name fast path first, then the live stack
        // marker for an agent whose thread description did not take.
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
    } // namespace

    std::expected<std::uint64_t, support::AllocationError>
    AgentService::allocate(support::Session& session, std::uint64_t size, std::uint64_t near_address)
    {
        const auto  call  = begin_call(session.pid);
        AgentState& state = call->state();
        if (session.suspended)
        {
            // Our own SIGSTOP pause stops the agent's threads too: answer with a
            // retryable busy error and never disable the agent.
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
            // No agent (yet): the host's /proc/maps view keeps the memory map
            // visible instead of failing the call.
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
            // Our own pause stops the agent's threads and the target cannot have
            // changed its loader list while stopped, so the recorded list stands.
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
                // A target stopped by the debugger cannot have changed its
                // loader list; the last list recorded for it still answers.
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

    std::expected<bool, support::AccessFault>
    AgentService::validate(support::Session& session, std::uint64_t address, std::size_t size)
    {
        const auto  call  = begin_call(session.pid);
        AgentState& state = call->state();
        if (session.suspended)
        {
            return support::range_is_readable(session, address, size);
        }

        const auto client = ensure_client(state, session.pid);
        if (!client)
        {
            return support::range_is_readable(session, address, size);
        }

        const auto info = client->query(address, kRequestTimeout);
        if (!info)
        {
            note_failure(client, state, session.pid, info.error());
            return support::range_is_readable(session, address, size);
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
        const auto  call  = begin_call(session.pid);
        AgentState& state = call->state();
        if (session.suspended)
        {
            return host_read(session, address, buffer);
        }

        const auto client = ensure_client(state, session.pid);
        if (!client)
        {
            return host_read(session, address, buffer);
        }

        const auto moved = client->read(address, buffer, kRequestTimeout);
        if (!moved)
        {
            // A failed exchange falls back to the path the host uses while the
            // target is stopped, so a read never dies with the agent.
            note_failure(client, state, session.pid, moved.error());
            return host_read(session, address, buffer);
        }
        return support::MemoryResult {*moved, SLOPKIT_ACCESS_AGENT};
    }

    std::expected<support::MemoryResult, support::AccessFault>
    AgentService::write(support::Session& session, std::uint64_t address, std::span<const std::byte> data)
    {
        const auto  call  = begin_call(session.pid);
        AgentState& state = call->state();
        if (session.suspended)
        {
            return host_write(session, address, data);
        }

        const auto client = ensure_client(state, session.pid);
        if (!client)
        {
            return host_write(session, address, data);
        }

        const auto moved = client->write(address, data, kRequestTimeout);
        if (!moved)
        {
            note_failure(client, state, session.pid, moved.error());
            return host_write(session, address, data);
        }
        return support::MemoryResult {*moved, SLOPKIT_ACCESS_AGENT};
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
            state->permanent = true; // no bring-up may start while closing
        }

        std::shared_ptr<AgentClient> client;
        {
            std::unique_lock lock(g_mutex);
            // Wait for every in-flight request before the region is unmapped.
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
            // SHUTDOWN frees what the agent allocated and unmaps the region; the
            // per-session directory goes with the transport.
            client->shutdown(kShutdownTimeout);
        }
    }

    std::vector<process::ProcessId> AgentService::agent_threads(support::Session& session)
    {
        auto found = scan_agent_threads(session, session.pid);
        {
            // Remember the last scan so the close path can clear it; the threads
            // are re-listed on every call because the agent creates workers
            // lazily and a stale list could miss one that would then be seized.
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

} // namespace slopkit::plugins::wine_proton
