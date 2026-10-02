#pragma once

#include <expected>
#include <string_view>
#include <vector>

#include "plugin/plugin_host.hpp"
#include "process/access.hpp"

namespace slopkit::process
{

    // Embedded ProcessAccess: reaches plugins loaded into this process through
    // dlopen. This is the seam where an out-of-process, IPC-based implementation
    // will replace the embedded one; the UI talks to ProcessAccess and nothing
    // below it, so the swap needs no UI change.
    class PluginAccess final : public ProcessAccess
    {
    public:
        explicit PluginAccess(plugin::PluginHost& host) noexcept : host_(host) {}

        ~PluginAccess() override = default;

        std::expected<std::vector<ProcessInfo>, AccessError> list_processes() override;
        std::expected<Session, AccessError>                  attach(ProcessId pid, std::string_view plugin_id) override;

    private:
        plugin::PluginHost& host_;
    };

} // namespace slopkit::process
