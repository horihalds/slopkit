#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "platform/linux/memory.hpp"
#include "platform/linux/ptrace.hpp"
#include "process/types.hpp"

namespace slopkit::platform
{
    enum class ForeignSignalPolicy
    {
        suppress,
        forward,
    };

    struct SoftwareSlot
    {
        bool          armed {};
        std::uint64_t address {};
        std::uint64_t original {};
        std::size_t   size {};
    };

    struct HardwareSlot
    {
        bool          armed {};
        std::int32_t  kind {};
        std::uint64_t address {};
        std::size_t   size {1};
    };

    struct DebugFrame
    {
        std::uint64_t pc {};
        std::uint64_t frame_pointer {};
    };

    inline constexpr std::size_t kSoftwareSlotCount = 4096;
    inline constexpr std::size_t kHardwareSlotCount = 4;

    class DebugSession
    {
    public:
        DebugSession(process::ProcessId pid, MemAccess& memory, ForeignSignalPolicy policy);
        DebugSession(const DebugSession&)            = delete;
        DebugSession& operator=(const DebugSession&) = delete;

        [[nodiscard]] bool               attached() const noexcept;
        [[nodiscard]] process::ProcessId leader() const noexcept;

        [[nodiscard]] std::expected<process::ProcessId, process::AccessError>
        attach(std::span<const process::ProcessId> excluded = {});
        [[nodiscard]] std::expected<void, process::AccessError> detach();

        [[nodiscard]] std::expected<void, process::AccessError> attach_thread(process::ProcessId tid);
        [[nodiscard]] std::expected<void, process::AccessError> detach_thread(process::ProcessId tid);
        [[nodiscard]] std::vector<process::ProcessId>           attached_threads() const;

        [[nodiscard]] std::expected<StopStatus, process::AccessError> resume(std::uint64_t resume_address);
        [[nodiscard]] std::expected<StopStatus, process::AccessError> step(process::ProcessId tid);
        [[nodiscard]] std::expected<StopStatus, process::AccessError> step_syscall(process::ProcessId tid);
        [[nodiscard]] std::expected<void, process::AccessError> continue_thread(process::ProcessId tid, int signal);
        [[nodiscard]] std::expected<std::optional<StopStatus>, process::AccessError> poll_stop(process::ProcessId tid);
        [[nodiscard]] std::expected<void, process::AccessError>                      interrupt(process::ProcessId tid);

        [[nodiscard]] std::expected<Registers, process::AccessError> registers(process::ProcessId tid);
        [[nodiscard]] std::expected<void, process::AccessError>
        set_register(process::ProcessId tid, std::string_view name, std::uint64_t value);

        [[nodiscard]] std::expected<void, process::AccessError>
        arm_software(std::uint32_t slot, std::uint64_t address, bool insert);
        [[nodiscard]] std::expected<void, process::AccessError>
        arm_hardware(std::uint32_t slot, std::int32_t kind, std::uint64_t address, std::size_t size, bool insert);

        [[nodiscard]] std::expected<std::vector<DebugFrame>, process::AccessError> backtrace(process::ProcessId tid);

        [[nodiscard]] bool holds_int3(std::uint64_t address);

    private:
        [[nodiscard]] std::expected<void, process::AccessError> apply_hardware();
        [[nodiscard]] std::expected<void, process::AccessError> ensure_stopped(process::ProcessId tid);
        void rewind_before_continue(process::ProcessId tid, std::uint64_t address);

        mutable std::mutex                           mutex_;
        std::vector<process::ProcessId>              tids_;
        std::unordered_set<process::ProcessId>       requested_stops_;
        std::array<SoftwareSlot, kSoftwareSlotCount> software_ {};
        std::array<HardwareSlot, kHardwareSlotCount> hardware_ {};
        process::ProcessId                           pid_ {};
        process::ProcessId                           leader_ {};
        process::ProcessId                           current_ {};
        bool                                         attached_ {};
        // True while `attach` seized the whole group, false for the
        // single-thread window: the two never coexist and only the group window
        // owns the debug registers.
        bool                                         group_window_ {};
        MemAccess&                                   memory_;
        ForeignSignalPolicy                          policy_ {ForeignSignalPolicy::suppress};
    };

}
