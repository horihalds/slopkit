#include <catch2/catch.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "plugins/wine_proton/agent/slopkit_agent_protocol.h"
#include "plugins/wine_proton/agent_client.hpp"
#include "plugins/wine_proton/agent_service.hpp"
#include "plugins/wine_proton/wine_target.hpp"

namespace
{
    using namespace std::chrono_literals;
    using slopkit::plugins::wine_proton::AgentClient;

    struct ChildGuard
    {
        pid_t pid {-1};

        explicit ChildGuard(pid_t child) : pid(child) {}

        ChildGuard(const ChildGuard&)            = delete;
        ChildGuard& operator=(const ChildGuard&) = delete;

        ChildGuard(ChildGuard&& other) noexcept : pid(std::exchange(other.pid, -1)) {}

        ~ChildGuard()
        {
            if (pid > 0)
            {
                ::kill(pid, SIGKILL);
                ::waitpid(pid, nullptr, 0);
            }
        }
    };

    std::filesystem::path find_wine()
    {
        for (const auto* candidate : {"/usr/bin/wine64", "/usr/bin/wine"})
        {
            std::error_code error;
            if (std::filesystem::exists(candidate, error))
            {
                return candidate;
            }
        }
        return {};
    }

    // Talks to the debug control block directly: one command at a time, each
    // published last and answered by the agent with the matching sequence.
    class DebugDriver
    {
    public:
        using Block = slopkit_agent_debug;

        DebugDriver(Block* block, AgentClient& client) : block_(block), client_(client)
        {
            sequence_ = static_cast<std::uint32_t>(std::atomic_ref<std::uint32_t>(block_->request).load());
        }

        bool
        send(std::int32_t cmd, std::uint64_t a0 = 0, std::uint64_t a1 = 0, std::uint64_t a2 = 0, std::uint64_t a3 = 0)
        {
            block_->arg[0] = a0;
            block_->arg[1] = a1;
            block_->arg[2] = a2;
            block_->arg[3] = a3;
            block_->status = SLOPKIT_AGENT_STATUS_OK;
            std::atomic_ref<std::int32_t>(block_->cmd).store(cmd, std::memory_order_relaxed);
            ++sequence_;
            std::atomic_ref<std::uint32_t>(block_->request).store(sequence_, std::memory_order_release);
            return client_.wait_for_answer(sequence_, 10s);
        }

        [[nodiscard]] std::uint32_t sequence() const noexcept
        {
            return sequence_;
        }

    private:
        Block*        block_;
        AgentClient&  client_;
        std::uint32_t sequence_ {0};
    };
}

// Hidden and gated like the allocation end-to-end test: it needs mingw-w64, a
// working wine prefix and the built DLL. It walks the debug core through the
// real Wine target the probe provides: attach and a frozen leader stop, a
// register read, a resume, a fresh interrupt and a clean detach.
//
// Run it explicitly with:
//   SLOPKIT_WINE_E2E=1 ./build/slopkit_tests "wine-proton debugs a target through the resident agent"
TEST_CASE("wine-proton debugs a target through the resident agent", "[wine-proton][agent][debug][.]")
{
    const char* enabled = std::getenv("SLOPKIT_WINE_E2E");
    if (enabled == nullptr || std::string(enabled) != "1")
    {
        WARN("set SLOPKIT_WINE_E2E=1 to run the wine end-to-end debug test");
        return;
    }

    const auto      wine  = find_wine();
    const auto      probe = std::filesystem::path(SLOPKIT_AGENT_PROBE_DIR) / "slopkit-probe-x86_64.exe";
    std::error_code error;
    if (wine.empty() || !std::filesystem::exists(probe, error))
    {
        WARN("wine or the 64-bit test probe is unavailable; the debug test is skipped");
        return;
    }
    ::setenv("SLOPKIT_AGENT_DIR", SLOPKIT_AGENT_BINARY_DIR, 1);

    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        ::execl(wine.c_str(), wine.c_str(), probe.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ChildGuard guard(child);
    std::this_thread::sleep_for(3s);

    const auto target = slopkit::plugins::wine_proton::derive_wine_target(child);
    if (!target.has_value())
    {
        WARN("the probe did not come up as a wine target; the debug test is skipped");
        return;
    }

    auto agent = AgentClient::launch(*target, 120s);
    if (!agent.has_value())
    {
        FAIL("the agent could not be launched: " << agent.error());
    }

    const auto offset = static_cast<std::uint32_t>(offsetof(slopkit_agent_region, debug));
    auto*      block  = agent->debug_at(offset);
    REQUIRE(block != nullptr);
    REQUIRE(block->size == sizeof(slopkit_agent_debug));
    REQUIRE(block->offset == offset);

    // The engine must leave the agent's own threads alone; the profile already
    // knows how to find them.
    slopkit::plugins::support::Session session(child, slopkit::platform::ForeignSignalPolicy::forward);
    const auto                         excluded = slopkit::plugins::wine_proton::AgentService::agent_threads(session);
    for (std::size_t index = 0; index < SLOPKIT_AGENT_DEBUG_MAX_EXCLUDED && index < excluded.size(); ++index)
    {
        block->excluded_tid[index] = excluded[index];
    }
    block->log_path[0] = '\0';

    std::atomic_ref<std::uint32_t>(block->state).store(SLOPKIT_AGENT_DBG_STATE_CLAIMED);

    const auto attached = agent->debug_attach(30s);
    REQUIRE(attached.has_value());
    REQUIRE(agent->wait_for_stop(30s));

    CHECK(block->stop_reason == SLOPKIT_AGENT_DBG_STOP_INTERRUPT);
    CHECK(block->held_linux_tid != 0);
    CHECK(block->register_mask != 0);
    const std::uint64_t leader_rip = block->registers.rip;
    CHECK(leader_rip != 0);

    DebugDriver driver(block, *agent);

    // Registers of the stopped leader.
    REQUIRE(driver.send(SLOPKIT_AGENT_DBG_CMD_GET_REGISTERS, block->held_linux_tid));
    CHECK(block->register_mask != 0);
    CHECK(block->registers.rip == leader_rip);

    // Release the interrupt stop: the process must run again.
    block->resume_address   = 0;
    block->resume_trap_size = 0xFFFFFFFFFFFFFFFFull;
    REQUIRE(driver.send(SLOPKIT_AGENT_DBG_CMD_RESUME, block->held_linux_tid));
    for (int spin = 0; spin < 2000; ++spin)
    {
        if (std::atomic_ref<std::uint32_t>(block->flag).load() == 0)
        {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    CHECK(std::atomic_ref<std::uint32_t>(block->flag).load() == 0);
    CHECK(std::atomic_ref<std::uint32_t>(block->state).load() == SLOPKIT_AGENT_DBG_STATE_RUNNING);

    // Interrupt again: the watchdog freezes the process and publishes a stop.
    REQUIRE(driver.send(SLOPKIT_AGENT_DBG_CMD_STOP));
    REQUIRE(agent->wait_for_stop(30s));
    CHECK(block->stop_reason == SLOPKIT_AGENT_DBG_STOP_INTERRUPT);
    CHECK(block->held_linux_tid != 0);

    // Clean detach: every thread thaws, every byte is restored.
    const auto detached = agent->debug_detach(30s);
    REQUIRE(detached.has_value());
    CHECK(std::atomic_ref<std::uint32_t>(block->state).load() == SLOPKIT_AGENT_DBG_STATE_DETACHED);

    // The probe is still alive after the whole dance.
    CHECK(::kill(child, 0) == 0);

    agent->shutdown(5s);
}
