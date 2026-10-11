#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "plugins/wine_proton/agent_transport.hpp"
#include "plugins/wine_proton/wine_target.hpp"

namespace slopkit::plugins::wine_proton
{
    inline constexpr std::uint32_t kAgentRequestTimeoutMs = 500;

    [[nodiscard]] std::string injector_failure_message(std::string_view output);

    struct AgentRegionInfo
    {
        std::uint64_t base {0};
        std::uint64_t size {0};
        std::uint32_t state {0};
        std::uint32_t protection {0};
    };

    struct AgentRegionEntry
    {
        std::uint64_t base {0};
        std::uint64_t size {0};
        std::uint32_t state {0};
        std::uint32_t protection {0};
        std::uint32_t type {0};
    };

    struct AgentRegionBatch
    {
        std::vector<AgentRegionEntry> regions;
        std::uint64_t                 next_cursor {0};
    };

    struct AgentModuleEntry
    {
        std::uint64_t base {0};
        std::uint64_t size {0};
        std::uint64_t entry {0};
        bool          main {false};
        std::string   name;
    };

    struct AgentModuleBatch
    {
        std::vector<AgentModuleEntry> modules;
        std::uint64_t                 next_index {0};
    };

    class AgentClient
    {
    public:
        [[nodiscard]] static std::string dll_file_name(std::uint32_t pointer_size);
        [[nodiscard]] static std::string injector_file_name(std::uint32_t pointer_size);

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
        [[nodiscard]] const std::filesystem::path& directory() const noexcept;
        [[nodiscard]] std::uint32_t                slot_count() const;
        [[nodiscard]] bool                         slots_exhausted() const;

        [[nodiscard]] std::expected<std::uint64_t, std::string>
        allocate(std::uint64_t size, std::uint64_t hint, std::chrono::milliseconds timeout);
        [[nodiscard]] std::expected<std::uint64_t, std::string>
        allocate_near(std::uint64_t size, std::uint64_t near_address, std::chrono::milliseconds timeout);
        [[nodiscard]] std::expected<void, std::string> free(std::uint64_t address, std::chrono::milliseconds timeout);
        [[nodiscard]] std::expected<std::uint32_t, std::string>
        protect(std::uint64_t address, std::uint64_t size, std::uint32_t protection, std::chrono::milliseconds timeout);
        [[nodiscard]] std::expected<AgentRegionInfo, std::string> query(std::uint64_t             address,
                                                                        std::chrono::milliseconds timeout);

        [[nodiscard]] std::expected<AgentRegionBatch, std::string>
        enumerate(std::uint64_t cursor, std::uint32_t capacity, std::chrono::milliseconds timeout);

        [[nodiscard]] std::expected<AgentModuleBatch, std::string>
        modules(std::uint64_t start_index, std::uint32_t capacity, std::chrono::milliseconds timeout);

        [[nodiscard]] std::expected<std::size_t, std::string>
        read(std::uint64_t address, std::span<std::byte> buffer, std::chrono::milliseconds timeout);

        [[nodiscard]] std::expected<std::size_t, std::string>
        write(std::uint64_t address, std::span<const std::byte> data, std::chrono::milliseconds timeout);

        void shutdown(std::chrono::milliseconds timeout = std::chrono::milliseconds {kAgentRequestTimeoutMs}) noexcept;

    private:
        AgentClient(std::shared_ptr<AgentTransport> transport, std::string injector_output);

        [[nodiscard]] std::expected<slopkit_agent_record, std::string> request(const slopkit_agent_record& record,
                                                                               std::chrono::milliseconds   timeout);

        [[nodiscard]] std::expected<slopkit_agent_record, std::string>
        transact(const slopkit_agent_record& record,
                 std::chrono::milliseconds   timeout,
                 std::span<const std::byte>  request_payload  = {},
                 std::span<std::byte>        response_payload = {});

        std::shared_ptr<AgentTransport>   transport_;
        mutable std::mutex                allocations_mutex_;
        std::unordered_set<std::uint64_t> allocations_;
        std::mutex                        shutdown_mutex_;
        std::string                       injector_output_;
        std::uint32_t                     windows_pid_ {0};
        std::uint32_t                     pointer_size_ {0};
        std::uint32_t                     granularity_ {0};
        std::uint64_t                     agent_base_ {0};
        bool                              shutdown_sent_ {false};
    };

}
