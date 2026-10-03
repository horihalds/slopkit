#include "plugin/plugin_host.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <system_error>
#include <utility>

#include "core/log.hpp"

namespace slopkit::plugin
{

    namespace
    {
        void* host_alloc(std::size_t size, void* /*user_data*/)
        {
            return std::malloc(size);
        }

        void host_dealloc(void* memory, void* /*user_data*/)
        {
            std::free(memory);
        }

        log::Level to_log_level(int32_t level)
        {
            switch (level)
            {
            case SLOPKIT_LOG_DEBUG:
                return log::Level::debug;
            case SLOPKIT_LOG_WARN:
                return log::Level::warning;
            case SLOPKIT_LOG_ERROR:
                return log::Level::error;
            case SLOPKIT_LOG_INFO:
            default:
                return log::Level::info;
            }
        }

        void host_log(int32_t level, const char* message, void* /*user_data*/)
        {
            if (message != nullptr)
            {
                log::Logger::instance().log(to_log_level(level), "plugin", message);
            }
        }
    } // namespace

    PluginHost::PluginHost()
    {
        host_.abi_version = SLOPKIT_PLUGIN_ABI_VERSION;
        host_.struct_size = sizeof(slopkit_host_services);
        host_.user_data   = nullptr;
        host_.alloc       = host_alloc;
        host_.dealloc     = host_dealloc;
        host_.log         = host_log;
    }

    PluginHost::~PluginHost() = default;

    std::vector<std::filesystem::path> PluginHost::split_search_path(std::string_view value)
    {
        std::vector<std::filesystem::path> paths;
        std::size_t                        start = 0;
        while (start <= value.size())
        {
            const auto separator = value.find(':', start);
            const auto length    = separator == std::string_view::npos ? std::string_view::npos : separator - start;
            const auto part      = value.substr(start, length);
            if (!part.empty())
            {
                paths.emplace_back(part);
            }
            if (separator == std::string_view::npos)
            {
                break;
            }
            start = separator + 1;
        }
        return paths;
    }

    std::vector<std::filesystem::path>
    PluginHost::default_search_directories(const std::filesystem::path& executable_dir)
    {
        std::vector<std::filesystem::path> directories;
        if (!executable_dir.empty())
        {
            // Build tree: plugins live next to the executable.
            directories.push_back(executable_dir / "plugins");
            // Installed layout: <prefix>/<libdir>/slopkit/plugins, resolved
            // exe-relative so any install prefix works without an environment
            // variable (see SLOPKIT_PLUGIN_RELATIVE_DIR).
            directories.push_back(executable_dir / SLOPKIT_PLUGIN_RELATIVE_DIR);
        }
        if (const char* path = std::getenv("SLOPKIT_PLUGIN_PATH"); path != nullptr)
        {
            auto extra = split_search_path(path);
            directories.insert(directories.end(), extra.begin(), extra.end());
        }
        return directories;
    }

    void PluginHost::discover(const std::vector<std::filesystem::path>& directories)
    {
        plugins_.clear();
        diagnostics_.clear();

        std::vector<std::filesystem::path>   seen;
        std::vector<std::unique_ptr<Plugin>> loaded;

        for (const auto& directory : directories)
        {
            std::error_code error;
            if (!std::filesystem::is_directory(directory, error))
            {
                continue; // A missing plugin directory is not fatal.
            }

            std::vector<std::filesystem::path> candidates;
            for (const auto& entry : std::filesystem::directory_iterator(directory, error))
            {
                if (error)
                {
                    break;
                }
                if (entry.is_regular_file() || entry.is_symlink())
                {
                    candidates.push_back(entry.path());
                }
            }
            std::sort(candidates.begin(), candidates.end());

            for (const auto& candidate : candidates)
            {
                std::error_code canonical_error;
                auto            canonical = std::filesystem::weakly_canonical(candidate, canonical_error);
                if (canonical_error)
                {
                    canonical = candidate;
                }
                if (std::find(seen.begin(), seen.end(), canonical) != seen.end())
                {
                    continue;
                }
                seen.push_back(canonical);

                auto plugin = Plugin::load(candidate, &host_);
                if (!plugin)
                {
                    diagnostics_.push_back(PluginDiagnostic {candidate, plugin.error()});
                    continue;
                }
                loaded.push_back(std::move(*plugin));
            }
        }

        // Most specific first: lower precedence wins; ties broken by id for
        // deterministic ordering.
        std::sort(loaded.begin(),
                  loaded.end(),
                  [](const std::unique_ptr<Plugin>& lhs, const std::unique_ptr<Plugin>& rhs)
                  {
                      if (lhs->precedence() != rhs->precedence())
                      {
                          return lhs->precedence() < rhs->precedence();
                      }
                      return lhs->id() < rhs->id();
                  });
        plugins_ = std::move(loaded);
    }

    std::span<const PluginDiagnostic> PluginHost::diagnostics() const noexcept
    {
        return diagnostics_;
    }

    std::span<const std::unique_ptr<Plugin>> PluginHost::plugins() const noexcept
    {
        return plugins_;
    }

    Plugin* PluginHost::find(std::string_view id) noexcept
    {
        for (const auto& plugin : plugins_)
        {
            if (plugin->id() == id)
            {
                return plugin.get();
            }
        }
        return nullptr;
    }

    const Plugin* PluginHost::find(std::string_view id) const noexcept
    {
        for (const auto& plugin : plugins_)
        {
            if (plugin->id() == id)
            {
                return plugin.get();
            }
        }
        return nullptr;
    }

    std::vector<process::ProcessInfo> PluginHost::list_processes()
    {
        std::vector<process::ProcessInfo>         merged;
        std::map<process::ProcessId, std::size_t> index_by_pid;

        for (const auto& plugin : plugins_)
        {
            auto processes = plugin->list_processes();
            if (!processes)
            {
                continue; // A plugin that cannot enumerate contributes nothing.
            }

            for (auto& process : *processes)
            {
                const auto found = index_by_pid.find(process.pid);
                if (found == index_by_pid.end())
                {
                    index_by_pid.emplace(process.pid, merged.size());
                    merged.push_back(std::move(process));
                    continue;
                }

                auto& existing = merged[found->second];
                existing.claimants.push_back(process.plugin_id);
                if (existing.name.empty())
                {
                    existing.name = std::move(process.name);
                }
                if (existing.exe_path.empty())
                {
                    existing.exe_path = std::move(process.exe_path);
                }
            }
        }

        std::sort(merged.begin(),
                  merged.end(),
                  [](const process::ProcessInfo& lhs, const process::ProcessInfo& rhs)
                  {
                      return lhs.pid < rhs.pid;
                  });
        return merged;
    }

} // namespace slopkit::plugin
