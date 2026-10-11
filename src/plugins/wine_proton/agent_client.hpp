#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "plugins/wine_proton/agent_transport.hpp"
#include "plugins/wine_proton/wine_target.hpp"

namespace slopkit::plugins::wine_proton
{

    // One request to the resident agent may take this long before the plugin
    // gives up on the agent path for the session.
    inline constexpr std::uint32_t kAgentRequestTimeoutMs = 500;

    // What QUERY reports about the target's own view of an address.
    struct AgentRegionInfo
    {
        std::uint64_t base {0};
        std::uint64_t size {0};
        std::uint32_t state {0};
        std::uint32_t protection {0};
    };

    // The Linux end of one injected agent: it runs the one-shot injector under
    // the target's own wine and prefix, completes the HELLO handshake, and then
    // serves the operations that have to go through Wine's VM manager.
    //
    // The agent tracks what it allocated itself, so FREE refuses a foreign
    // address; `owns()` mirrors that set on this side so the plugin can route a
    // free to the right path. The destructor sends SHUTDOWN, which frees what
    // the agent allocated, and unmaps the shared region.
    class AgentClient
    {
    public:
        // The file names the wine-proton build produces per bitness.
        [[nodiscard]] static std::string dll_file_name(std::uint32_t pointer_size);
        [[nodiscard]] static std::string injector_file_name(std::uint32_t pointer_size);

        // Injects the agent matching the target's bitness and handshakes with
        // it. Any failure leaves the target untouched, so the caller can fall
        // back to ptrace with the returned reason.
        [[nodiscard]] static std::expected<AgentClient, std::string> launch(const WineTarget&         target,
                                                                            std::chrono::milliseconds timeout);

        AgentClient(AgentClient&& other) noexcept;
        AgentClient& operator=(AgentClient&& other) noexcept;
        AgentClient(const AgentClient&)            = delete;
        AgentClient& operator=(const AgentClient&) = delete;
        ~AgentClient();

        [[nodiscard]] std::uint32_t                windows_pid() const noexcept;
        [[nodiscard]] std::uint32_t                granularity() const noexcept;
        [[nodiscard]] std::uint64_t                agent_base() const noexcept;
        [[nodiscard]] const std::string&           injector_output() const noexcept;
        [[nodiscard]] bool                         owns(std::uint64_t address) const noexcept;
        // The page-rounded size of an address this client allocated, so the
        // ptrace fallback can still unmap it when the agent has been given up
        // on. std::nullopt for an address the agent did not hand out.
        [[nodiscard]] std::optional<std::uint64_t> allocation_size(std::uint64_t address) const noexcept;
        // The directory holding the region, the DLL and the injector.
        [[nodiscard]] const std::filesystem::path& directory() const noexcept;

        [[nodiscard]] std::expected<std::uint64_t, std::string>
        allocate(std::uint64_t size, std::uint64_t hint, std::chrono::milliseconds timeout);
        [[nodiscard]] std::expected<std::uint64_t, std::string>
        allocate_near(std::uint64_t size, std::uint64_t near_address, std::chrono::milliseconds timeout);
        [[nodiscard]] std::expected<void, std::string> free(std::uint64_t address, std::chrono::milliseconds timeout);
        [[nodiscard]] std::expected<std::uint32_t, std::string>
        protect(std::uint64_t address, std::uint64_t size, std::uint32_t protection, std::chrono::milliseconds timeout);
        [[nodiscard]] std::expected<AgentRegionInfo, std::string> query(std::uint64_t             address,
                                                                        std::chrono::milliseconds timeout);

        // Frees what the agent allocated and stops it serving. Never throws and
        // never blocks longer than `timeout`.
        void shutdown(std::chrono::milliseconds timeout = std::chrono::milliseconds {kAgentRequestTimeoutMs}) noexcept;

    private:
        AgentClient(std::shared_ptr<AgentTransport> transport, std::string injector_output);

        [[nodiscard]] std::expected<slopkit_agent_record, std::string> request(const slopkit_agent_record& record,
                                                                               std::chrono::milliseconds   timeout);

        std::shared_ptr<AgentTransport>                  transport_;
        std::unordered_map<std::uint64_t, std::uint64_t> allocations_;
        std::string                                      injector_output_;
        std::uint32_t                                    windows_pid_ {0};
        std::uint32_t                                    pointer_size_ {0};
        std::uint32_t                                    granularity_ {0};
        std::uint64_t                                    agent_base_ {0};
        bool                                             shutdown_sent_ {false};
    };

} // namespace slopkit::plugins::wine_proton
