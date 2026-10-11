#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>

#include "plugins/wine_proton/agent/slopkit_agent_protocol.h"

namespace slopkit::plugins::wine_proton
{

    // The Linux end of the file-backed shared region the resident agent polls.
    //
    // `create()` makes the per-session directory, creates the region file there
    // and mmaps it; the agent maps the same file through Wine's Z: drive as the
    // sibling of the DLL. The header's state word is the only synchronisation,
    // and every wait is bounded so a hung or dead agent can never hang the
    // tool: `transact()` returns a timeout error and the caller reports it.
    //
    // The region file lives beside the agent DLL and injector because the DLL
    // looks for it next to its own module path; that is also why the destructor
    // removes the whole directory.
    class AgentTransport
    {
    public:
        static std::expected<AgentTransport, std::string> create(const std::filesystem::path& directory);

        AgentTransport(const AgentTransport&)            = delete;
        AgentTransport& operator=(const AgentTransport&) = delete;
        AgentTransport(AgentTransport&& other) noexcept;
        AgentTransport& operator=(AgentTransport&& other) noexcept;
        ~AgentTransport();

        [[nodiscard]] const std::filesystem::path& directory() const noexcept;
        // The directory in the form the target sees it (Z:\...).
        [[nodiscard]] std::string                  windows_directory() const;

        [[nodiscard]] const slopkit_agent_header& header() const noexcept;
        [[nodiscard]] std::uint32_t               load_state() const noexcept;
        void                                      store_state(std::uint32_t value) noexcept;

        // Publishes `request` and waits up to `timeout` for the agent's answer.
        // The heartbeat is bumped while waiting so an agent that has backed off
        // wakes quickly; an error names the reason (timeout, a protocol
        // mismatch the agent reported, or a reply that does not belong to this
        // request).
        //
        // For the payload-carrying ops, `request_payload` is copied into the
        // region before the request is published and up to `response_payload`
        // bytes of the answer are copied out before the state returns to idle,
        // so the next request never races the previous answer's payload.
        [[nodiscard]] std::expected<slopkit_agent_record, std::string>
        transact(const slopkit_agent_record& request,
                 std::chrono::milliseconds   timeout,
                 std::span<const std::byte>  request_payload  = {},
                 std::span<std::byte>        response_payload = {});

    private:
        AgentTransport(std::filesystem::path directory, int descriptor, slopkit_agent_region* region) noexcept;

        std::filesystem::path directory_;
        int                   descriptor_ {-1};
        slopkit_agent_region* region_ {nullptr};
        std::uint32_t         request_id_ {0};
        std::uint32_t         heartbeat_ {0};
    };

} // namespace slopkit::plugins::wine_proton
