#pragma once

#include <expected>
#include <optional>
#include <string_view>

#include "debug/backend.hpp"
#include "plugin/plugin.hpp"
#include "process/types.hpp"

namespace slopkit::plugin
{
    class PluginHost;
}

namespace slopkit::debug
{

    // DebugBackend over plugin::PluginHost: opens its own plugin session, calls
    // the ABI 1.4 debug operations and maps them onto the host types. A plugin
    // that leaves the debug pointers null reports `unsupported`.
    class PluginBackend final : public DebugBackend
    {
    public:
        explicit PluginBackend(plugin::PluginHost& host);
        ~PluginBackend() override;

        PluginBackend(const PluginBackend&)            = delete;
        PluginBackend& operator=(const PluginBackend&) = delete;

        std::expected<void, process::AccessError> attach(process::ProcessId pid, std::string_view plugin_id) override;
        std::expected<void, process::AccessError> detach() override;
        std::expected<StopEvent, process::AccessError> cont(std::uint64_t resume_address,
                                                            std::size_t   resume_step_size) override;
        std::expected<StopEvent, process::AccessError> step(std::uint32_t tid) override;
        std::expected<void, process::AccessError>      interrupt(std::uint32_t tid) override;

        std::expected<std::vector<RegisterValue>, process::AccessError> registers(std::uint32_t tid) override;
        std::expected<void, process::AccessError>
        set_register(std::uint32_t tid, std::string_view name, std::uint64_t value) override;
        std::expected<void, process::AccessError>
        set_software_breakpoint(std::uint32_t slot, std::uint64_t address, bool insert) override;
        std::expected<void, process::AccessError> set_hardware_breakpoint(
            std::uint32_t slot, HardwareKind kind, std::uint64_t address, std::size_t size, bool insert) override;
        std::expected<std::vector<Frame>, process::AccessError> backtrace(std::uint32_t tid) override;

        [[nodiscard]] bool supports_debug() const override;

    private:
        [[nodiscard]] bool                         open() const noexcept;
        [[nodiscard]] const slopkit_plugin_vtable* vtable() const noexcept;

        plugin::PluginHost&                  host_;
        std::optional<plugin::PluginSession> session_;
    };

} // namespace slopkit::debug
