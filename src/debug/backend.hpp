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

    // One register as the debugger shows it.
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

    // One stop of a traced thread. `trap_address` is the int3 address of a
    // software trap or the watched address of a hardware breakpoint; the DR slot
    // is set for a hardware hit.
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

    // One raw stack frame; all labelling stays in the host.
    struct Frame
    {
        std::uint64_t pc {};
        std::uint64_t frame_pointer {};
    };

    // The seam the controller and the unit tests depend on, mirroring
    // process::SessionBackend: a fake drives the tests, PluginBackend wraps the
    // linux-proc plugin.
    class DebugBackend
    {
    public:
        virtual ~DebugBackend() = default;

        // Opens the debug session for `pid` through the named plugin.
        virtual std::expected<void, process::AccessError>      attach(process::ProcessId pid,
                                                                      std::string_view   plugin_id) = 0;
        virtual std::expected<void, process::AccessError>      detach()                             = 0;
        // Resumes and blocks until a stop. A non-zero `resume_address` means the
        // software trap there must be lifted for one instruction.
        virtual std::expected<StopEvent, process::AccessError> cont(std::uint64_t resume_address,
                                                                    std::size_t   resume_step_size) = 0;
        virtual std::expected<StopEvent, process::AccessError> step(std::uint32_t tid)              = 0;
        // Asks a running thread to stop without waiting.
        virtual std::expected<void, process::AccessError>      interrupt(std::uint32_t tid)         = 0;

        virtual std::expected<std::vector<RegisterValue>, process::AccessError> registers(std::uint32_t tid) = 0;
        virtual std::expected<void, process::AccessError>
        set_register(std::uint32_t tid, std::string_view name, std::uint64_t value) = 0;
        virtual std::expected<void, process::AccessError>
        set_software_breakpoint(std::uint32_t slot, std::uint64_t address, bool insert) = 0;
        virtual std::expected<void, process::AccessError> set_hardware_breakpoint(
            std::uint32_t slot, HardwareKind kind, std::uint64_t address, std::size_t size, bool insert) = 0;
        virtual std::expected<std::vector<Frame>, process::AccessError> backtrace(std::uint32_t tid)     = 0;

        // False when the plugin leaves the debug operations null; Start then
        // reports "unsupported".
        [[nodiscard]] virtual bool supports_debug() const = 0;
    };

} // namespace slopkit::debug
