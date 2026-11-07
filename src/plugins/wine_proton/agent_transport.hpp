#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>

#include "plugins/wine_proton/agent/slopkit_agent_protocol.h"

namespace slopkit::plugins::wine_proton
{
    class AgentTransport
    {
    public:
        class Slot
        {
        public:
            Slot(const Slot&)            = delete;
            Slot& operator=(const Slot&) = delete;
            Slot(Slot&& other) noexcept;
            Slot& operator=(Slot&& other) noexcept;
            ~Slot();

            [[nodiscard]] std::uint32_t index() const noexcept;

            [[nodiscard]] std::expected<slopkit_agent_record, std::string>
            transact(const slopkit_agent_record& request,
                     std::chrono::milliseconds   timeout,
                     std::span<const std::byte>  request_payload  = {},
                     std::span<std::byte>        response_payload = {});

        private:
            friend class AgentTransport;
            Slot(AgentTransport& transport, std::uint32_t index) noexcept;
            void retire() noexcept;

            AgentTransport* transport_ {nullptr};
            std::uint32_t   index_ {0};
        };

        static std::expected<AgentTransport, std::string> create(const std::filesystem::path& directory);

        AgentTransport(const AgentTransport&)            = delete;
        AgentTransport& operator=(const AgentTransport&) = delete;
        AgentTransport(AgentTransport&& other) noexcept;
        AgentTransport& operator=(AgentTransport&& other) noexcept;
        ~AgentTransport();

        [[nodiscard]] const std::filesystem::path& directory() const noexcept;
        [[nodiscard]] std::string                  windows_directory() const;

        [[nodiscard]] const slopkit_agent_header& header() const noexcept;
        [[nodiscard]] slopkit_agent_slot&         slot(std::uint32_t index) noexcept;
        [[nodiscard]] const slopkit_agent_slot&   slot(std::uint32_t index) const noexcept;
        [[nodiscard]] std::uint32_t               load_slot_state(std::uint32_t index) const noexcept;
        void                                      store_slot_state(std::uint32_t index, std::uint32_t value) noexcept;

        [[nodiscard]] std::uint32_t slot_count() const;
        [[nodiscard]] std::uint32_t available_slots() const;
        [[nodiscard]] std::uint32_t retired_slots() const;
        [[nodiscard]] bool          exhausted() const;

        void advertise_capacity(std::uint32_t slots) noexcept;

        [[nodiscard]] std::expected<Slot, std::string> acquire(std::chrono::milliseconds timeout);

        /* The mapped region itself, so the debug block can be driven directly
           by the debug backend instead of through the request slots. */
        [[nodiscard]] slopkit_agent_region* region() const noexcept;

    private:
        AgentTransport(std::filesystem::path directory, int descriptor, slopkit_agent_region* region) noexcept;

        void release(std::uint32_t index) noexcept;
        void retire(std::uint32_t index) noexcept;

        std::filesystem::path directory_;
        int                   descriptor_ {-1};
        slopkit_agent_region* region_ {nullptr};

        mutable std::mutex                         mutex_;
        std::condition_variable                    ready_;
        std::uint32_t                              pool_size_ {1};
        std::uint32_t                              available_ {1};
        std::uint32_t                              retired_in_pool_ {0};
        std::array<bool, SLOPKIT_AGENT_SLOT_COUNT> in_use_ {};
        std::array<bool, SLOPKIT_AGENT_SLOT_COUNT> retired_ {};
        std::atomic<std::uint32_t>                 request_id_ {0};
        std::atomic<std::uint32_t>                 heartbeat_ {0};
    };

}
