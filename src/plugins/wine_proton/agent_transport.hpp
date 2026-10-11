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

    // The Linux end of the file-backed shared region the resident agent polls.
    //
    // `create()` makes the per-session directory, creates the region file there
    // and mmaps it; the agent maps the same file through Wine's Z: drive as the
    // sibling of the DLL. The region holds a ring of independent request slots
    // and each slot's state word is the synchronisation for that slot, so
    // several host threads can transact in parallel without corrupting each
    // other. Every wait is bounded so a hung or dead agent can never hang the
    // tool: `Slot::transact()` returns a timeout error and the caller reports
    // it, and a timed-out slot is retired so a late answer is never mistaken
    // for the next request's answer.
    //
    // The region file lives beside the agent DLL and injector because the DLL
    // looks for it next to its own module path; that is also why the destructor
    // removes the whole directory.
    class AgentTransport
    {
    public:
        // One claimed request slot. Its lifetime is the claim: the slot returns
        // to the pool when the handle is destroyed (move it back or let it fall
        // out of scope), and a slot whose exchange timed out is retired instead
        // and never reused.
        class Slot
        {
        public:
            Slot(const Slot&)            = delete;
            Slot& operator=(const Slot&) = delete;
            Slot(Slot&& other) noexcept;
            Slot& operator=(Slot&& other) noexcept;
            ~Slot();

            [[nodiscard]] std::uint32_t index() const noexcept;

            // Publishes `request` on this slot and waits up to `timeout` for the
            // agent's answer. The heartbeat is bumped while waiting so an agent
            // that has backed off wakes quickly; an error names the reason
            // (timeout, a protocol mismatch the agent reported, or a reply that
            // does not belong to this request). A timeout retires the slot.
            //
            // For the payload-carrying ops, `request_payload` is copied into the
            // slot before the request is published and up to `response_payload`
            // bytes of the answer are copied out before the state returns to
            // idle, so the next request never races the previous answer.
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
        // The directory in the form the target sees it (Z:\...).
        [[nodiscard]] std::string                  windows_directory() const;

        [[nodiscard]] const slopkit_agent_header& header() const noexcept;
        // Raw slot access for the mock agent that drives the wire in tests.
        [[nodiscard]] slopkit_agent_slot&         slot(std::uint32_t index) noexcept;
        [[nodiscard]] const slopkit_agent_slot&   slot(std::uint32_t index) const noexcept;
        [[nodiscard]] std::uint32_t               load_slot_state(std::uint32_t index) const noexcept;
        void                                      store_slot_state(std::uint32_t index, std::uint32_t value) noexcept;

        // The pool capacity the agent advertised; 1 when it reported none.
        [[nodiscard]] std::uint32_t slot_count() const;
        // Slots still free and not retired.
        [[nodiscard]] std::uint32_t available_slots() const;
        // Slots poisoned by a timed-out exchange, within the advertised pool.
        [[nodiscard]] std::uint32_t retired_slots() const;
        // True once every slot in the pool has been retired, so no request can
        // ever be served on this transport again and a fresh agent is needed.
        [[nodiscard]] bool          exhausted() const;

        // Sizes the pool from the agent's HELLO answer; 0 keeps the single-slot
        // degradation so an older or quieter agent still works.
        void advertise_capacity(std::uint32_t slots) noexcept;

        // Takes a slot out of the pool, waiting up to `timeout` for one to come
        // free. Capacity comes from `advertise_capacity`.
        [[nodiscard]] std::expected<Slot, std::string> acquire(std::chrono::milliseconds timeout);

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

} // namespace slopkit::plugins::wine_proton
