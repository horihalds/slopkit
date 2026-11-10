#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <mpfshost/client.hpp>

#include "platform/linux/debug_session.hpp"
#include "plugins/mpfshost/mpfshost_helpers.hpp"
#include "process/types.hpp"

namespace slopkit::plugins::mpfshost
{
    class DebugManager
    {
    public:
        [[nodiscard]] std::expected<process::ProcessId, process::AccessError> attach(const GuestTarget& guest,
                                                                                     std::uint32_t      synthetic_pid);
        [[nodiscard]] std::expected<void, process::AccessError>               detach(std::uint32_t synthetic_pid);

        [[nodiscard]] std::expected<platform::StopStatus, process::AccessError>
        resume(std::uint32_t synthetic_pid, std::uint64_t resume_address, std::size_t resume_step_size);
        [[nodiscard]] std::expected<platform::StopStatus, process::AccessError> step(std::uint32_t synthetic_pid,
                                                                                     std::uint32_t tid);
        [[nodiscard]] std::expected<void, process::AccessError>                 interrupt(const GuestTarget& guest,
                                                                                          std::uint32_t      synthetic_pid);

        [[nodiscard]] std::expected<platform::Registers, process::AccessError> registers(std::uint32_t synthetic_pid,
                                                                                         std::uint32_t tid);
        [[nodiscard]] std::expected<void, process::AccessError>
        set_register(std::uint32_t synthetic_pid, std::uint32_t tid, std::string_view name, std::uint64_t value);
        [[nodiscard]] std::expected<void, process::AccessError>
        arm_software(std::uint32_t synthetic_pid, std::uint32_t slot, std::uint64_t address, bool insert);
        [[nodiscard]] std::expected<void, process::AccessError> arm_hardware(std::uint32_t synthetic_pid,
                                                                             std::uint32_t slot,
                                                                             std::int32_t  kind,
                                                                             std::uint64_t address,
                                                                             std::size_t   size,
                                                                             bool          insert);
        [[nodiscard]] std::expected<std::vector<platform::DebugFrame>, process::AccessError>
        backtrace(std::uint32_t synthetic_pid, std::uint32_t tid);

        [[nodiscard]] std::expected<void, process::AccessError> suspend(const GuestTarget& guest);
        [[nodiscard]] std::expected<void, process::AccessError> resume_target(const GuestTarget& guest);

        void forget(std::uint32_t synthetic_pid);

    private:
        struct Resource
        {
            std::uint32_t id {};
            bool          watchpoint {};
        };

        struct State
        {
            std::mutex                                  mutex;
            // Serializes a maintenance stop with the continue packet it must follow.
            std::mutex                                  continue_mutex;
            std::optional<::mpfshost::ClientSession>    session;
            std::uint32_t                               guest_pid {};
            std::uint32_t                               leader_tid {};
            std::unordered_map<std::uint64_t, Resource> resources;
        };

        [[nodiscard]] std::shared_ptr<State> state_for(std::uint32_t synthetic_pid);

        std::mutex                                                mutex_;
        std::unordered_map<std::uint32_t, std::shared_ptr<State>> states_;
    };
}
