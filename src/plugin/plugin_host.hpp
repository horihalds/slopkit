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

    // A library that was found but rejected during discovery.
    struct PluginDiagnostic
    {
        std::filesystem::path path;
        std::string           message;
    };

    // Discovers and owns the loaded plugins.
    class PluginHost
    {
    public:
        PluginHost();
        ~PluginHost();
        PluginHost(const PluginHost&)            = delete;
        PluginHost& operator=(const PluginHost&) = delete;
        PluginHost(PluginHost&&)                 = delete;
        PluginHost& operator=(PluginHost&&)      = delete;

        // `${executable_dir}/plugins`, the installed plugin directory
        // (`${executable_dir}/../<libdir>/slopkit/plugins`), then every entry of
        // the colon-separated SLOPKIT_PLUGIN_PATH environment variable.
        static std::vector<std::filesystem::path>
        default_search_directories(const std::filesystem::path& executable_dir);
        static std::vector<std::filesystem::path> split_search_path(std::string_view value);

        void discover(const std::vector<std::filesystem::path>& directories);

        [[nodiscard]] std::span<const PluginDiagnostic>        diagnostics() const noexcept;
        [[nodiscard]] std::span<const std::unique_ptr<Plugin>> plugins() const noexcept;
        [[nodiscard]] Plugin*                                  find(std::string_view id) noexcept;
        [[nodiscard]] const Plugin*                            find(std::string_view id) const noexcept;

        // Merged listing over every plugin, sorted by pid. Each entry records all
        // of its claimants and defaults to the most specific one (lowest
        // precedence).
        [[nodiscard]] std::vector<process::ProcessInfo> list_processes();

    private:
        std::vector<std::unique_ptr<Plugin>> plugins_;
        std::vector<PluginDiagnostic>        diagnostics_;
    };

} // namespace slopkit::plugin
