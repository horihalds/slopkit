#pragma once

#include <expected>
#include <string_view>
#include <vector>

#include "plugin/plugin_host.hpp"
#include "process/access.hpp"

namespace slopkit::process
{
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

}
