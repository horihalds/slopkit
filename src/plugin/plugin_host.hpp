#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "plugin/plugin.hpp"
#include "process/types.hpp"

namespace slopkit::plugin
{
    struct PluginDiagnostic
    {
        std::filesystem::path path;
        std::string           message;
    };

    class PluginHost
    {
    public:
        PluginHost();
        ~PluginHost();
        PluginHost(const PluginHost&)            = delete;
        PluginHost& operator=(const PluginHost&) = delete;
        PluginHost(PluginHost&&)                 = delete;
        PluginHost& operator=(PluginHost&&)      = delete;

        static std::vector<std::filesystem::path>
        default_search_directories(const std::filesystem::path& executable_dir);
        static std::vector<std::filesystem::path> split_search_path(std::string_view value);

        void discover(const std::vector<std::filesystem::path>& directories);

        [[nodiscard]] std::span<const PluginDiagnostic>        diagnostics() const noexcept;
        [[nodiscard]] std::span<const std::unique_ptr<Plugin>> plugins() const noexcept;
        [[nodiscard]] Plugin*                                  find(std::string_view id) noexcept;
        [[nodiscard]] const Plugin*                            find(std::string_view id) const noexcept;

        [[nodiscard]] std::vector<process::ProcessInfo> list_processes();

    private:
        std::vector<std::unique_ptr<Plugin>> plugins_;
        std::vector<PluginDiagnostic>        diagnostics_;
    };

}
