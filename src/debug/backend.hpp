#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "process/types.hpp"

namespace slopkit::debug
{
    struct RegisterValue
    {
        std::string   name;
        std::uint64_t value {};
    };

    enum class StopReason
    {
        breakpoint,
        single_step,
        interrupt,
        exited,
        signalled,
    };

    struct StopEvent
    {
        StopReason                   reason {StopReason::interrupt};
        std::uint32_t                tid {};
        std::uint64_t                address {};
        std::uint64_t                trap_address {};
        std::optional<std::uint32_t> breakpoint_slot;
        std::uint32_t                signal {};
    };

    enum class HardwareKind
    {
        execute,
        write,
        read_write,
    };

    struct Frame
    {
        std::uint64_t pc {};
        std::uint64_t frame_pointer {};
    };

    class DebugBackend
    {
    public:
        virtual ~DebugBackend() = default;

        virtual std::expected<void, process::AccessError>      attach(process::ProcessId pid,
                                                                      std::string_view   plugin_id) = 0;
        virtual std::expected<void, process::AccessError>      detach()                             = 0;
        virtual std::expected<StopEvent, process::AccessError> cont(std::uint64_t resume_address,
                                                                    std::size_t   resume_step_size) = 0;
        virtual std::expected<StopEvent, process::AccessError> step(std::uint32_t tid)              = 0;
        virtual std::expected<void, process::AccessError>      interrupt(std::uint32_t tid)         = 0;

        virtual std::expected<std::vector<RegisterValue>, process::AccessError> registers(std::uint32_t tid) = 0;
        virtual std::expected<void, process::AccessError>
        set_register(std::uint32_t tid, std::string_view name, std::uint64_t value) = 0;
        virtual std::expected<void, process::AccessError>
        set_software_breakpoint(std::uint32_t slot, std::uint64_t address, bool insert) = 0;
        virtual std::expected<void, process::AccessError> set_hardware_breakpoint(
            std::uint32_t slot, HardwareKind kind, std::uint64_t address, std::size_t size, bool insert) = 0;
        virtual std::expected<std::vector<Frame>, process::AccessError> backtrace(std::uint32_t tid)     = 0;

        [[nodiscard]] virtual bool supports_debug() const = 0;
    };

}
