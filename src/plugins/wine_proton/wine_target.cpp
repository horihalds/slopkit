#include "plugins/wine_proton/wine_target.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

#include <unistd.h>

#include "platform/linux/procfs.hpp"
#include "platform/linux/wine.hpp"

namespace slopkit::plugins::wine_proton
{
    namespace
    {
        std::vector<std::filesystem::path> wine_candidates(process::ProcessId                pid,
                                                           const std::optional<std::string>& exe)
        {
            std::vector<std::filesystem::path> candidates;
            const auto                         add = [&candidates](const std::filesystem::path& candidate)
            {
                if (!candidate.empty() && std::ranges::find(candidates, candidate) == candidates.end())
                {
                    candidates.push_back(candidate);
                }
            };

            if (exe)
            {
                const std::filesystem::path path(*exe);
                const std::string           name = path.filename().string();
                if (name.starts_with("wine"))
                {
                    constexpr std::string_view kPreloader = "-preloader";
                    if (name.ends_with(kPreloader))
                    {
                        std::string loader = name;
                        loader.resize(loader.size() - kPreloader.size());
                        add(path.parent_path() / loader);
                    }
                    else
                    {
                        add(path);
                    }
                }
                add(path.parent_path() / "wine64");
                add(path.parent_path() / "wine");
            }

            for (const auto& region : platform::read_maps(pid))
            {
                if (region.path.empty() || region.path.front() != '/')
                {
                    continue;
                }
                for (const std::string_view marker : {"/lib64/wine/", "/lib32/wine/", "/lib/wine/"})
                {
                    const auto at = region.path.find(marker);
                    if (at == std::string::npos)
                    {
                        continue;
                    }
                    const std::filesystem::path root(region.path.substr(0, at));
                    add(root / "bin" / "wine64");
                    add(root / "bin" / "wine");
                }
            }

            add("/usr/bin/wine64");
            add("/usr/bin/wine");
            return candidates;
        }

        std::filesystem::path pick_wine_binary(const std::vector<std::filesystem::path>& candidates)
        {
            for (const auto& candidate : candidates)
            {
                std::error_code error;
                if (std::filesystem::exists(candidate, error) && !error)
                {
                    return candidate;
                }
            }
            return {};
        }

        bool is_process_local_handoff(const std::string& entry)
        {
            const auto             separator = entry.find('=');
            const std::string_view name =
                separator == std::string::npos ? std::string_view(entry) : std::string_view(entry).substr(0, separator);
            return name == "WINESERVERSOCKET";
        }

        std::vector<std::filesystem::path> staging_base_candidates()
        {
            std::vector<std::filesystem::path> candidates;
            if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime != nullptr && *runtime != '\0')
            {
                candidates.emplace_back(runtime);
            }
            std::error_code error;
            const auto      temporary = std::filesystem::temp_directory_path(error);
            if (!error)
            {
                candidates.push_back(temporary);
            }
            if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
            {
                candidates.push_back(std::filesystem::path(home) / ".cache" / "slopkit");
            }
            return candidates;
        }

        std::string join(const std::vector<std::string>& parts)
        {
            std::string result;
            for (const auto& part : parts)
            {
                if (!result.empty())
                {
                    result += "; ";
                }
                result += part;
            }
            return result;
        }
    }

    std::optional<std::string> guest_image_name(process::ProcessId pid)
    {
        return platform::guest_image_name(pid);
    }

    std::uint32_t guest_pointer_size(process::ProcessId pid)
    {
        return platform::guest_pointer_size(pid);
    }

    std::string linux_path_to_windows(const std::string& path)
    {
        std::string result = "Z:";
        result.reserve(path.size() + 2);
        for (const char character : path)
        {
            result.push_back(character == '/' ? '\\' : character);
        }
        return result;
    }

    std::vector<std::string> injector_environment(const std::vector<std::string>& environment)
    {
        std::vector<std::string> filtered;
        filtered.reserve(environment.size());
        for (const auto& entry : environment)
        {
            if (!is_process_local_handoff(entry))
            {
                filtered.push_back(entry);
            }
        }
        return filtered;
    }

    std::vector<std::string>
    injector_allocate_arguments(const WineTarget& target, const std::filesystem::path& injector, std::size_t bytes)
    {
        return {target.wine_binary.string(),
                injector.string(),
                "--image",
                target.image_path,
                "--allocate",
                std::to_string(bytes)};
    }

    std::vector<std::string> injector_load_arguments(const WineTarget&                 target,
                                                     const std::filesystem::path&      injector,
                                                     std::uint64_t                     address,
                                                     const std::optional<std::string>& windows_dll_path)
    {
        std::vector<std::string> arguments {target.wine_binary.string(),
                                            injector.string(),
                                            "--image",
                                            target.image_path,
                                            "--load",
                                            std::to_string(address)};
        if (windows_dll_path)
        {
            arguments.push_back("--dll");
            arguments.push_back(*windows_dll_path);
        }
        return arguments;
    }

    bool target_can_reach(process::ProcessId pid, const std::filesystem::path& path)
    {
        if (path.is_relative())
        {
            return false;
        }
        std::error_code error;
        const auto through_root = std::filesystem::path("/proc") / std::to_string(pid) / "root" / path.relative_path();
        return std::filesystem::exists(through_root, error) && !error;
    }

    std::expected<std::filesystem::path, std::string> make_staging_directory(process::ProcessId pid)
    {
        const std::string        name = std::format("slopkit-agent-{}-{}", pid, static_cast<long>(::getpid()));
        std::vector<std::string> rejected;
        for (const auto& base : staging_base_candidates())
        {
            const auto      directory = base / name;
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            if (error)
            {
                rejected.push_back(std::format("{} ({})", directory.string(), error.message()));
                continue;
            }
            if (target_can_reach(pid, directory))
            {
                return directory;
            }
            rejected.push_back(std::format("{} (the target cannot reach it)", directory.string()));
            std::error_code cleanup;
            std::filesystem::remove_all(directory, cleanup);
        }
        return std::unexpected(std::format("no staging directory the target can reach; tried {}", join(rejected)));
    }

    std::expected<WineTarget, std::string> derive_wine_target(process::ProcessId pid)
    {
        const auto image = platform::guest_image(pid);
        if (!image)
        {
            return std::unexpected(std::string("the target's main PE image could not be identified"));
        }

        const auto wine_binary = pick_wine_binary(wine_candidates(pid, platform::read_exe(pid)));
        if (wine_binary.empty())
        {
            return std::unexpected(std::string("no wine binary could be found for the target"));
        }

        WineTarget target;
        target.pid          = pid;
        target.pointer_size = image->pointer_size;
        target.wine_binary  = wine_binary;
        target.environment  = injector_environment(platform::read_environ(pid));
        target.image_path   = linux_path_to_windows(image->path);
        if (const auto exe = platform::read_exe(pid))
        {
            target.exe_path = *exe;
        }

        const bool has_prefix = std::ranges::any_of(target.environment,
                                                    [](const std::string& entry)
                                                    {
                                                        return entry.starts_with("WINEPREFIX=");
                                                    });
        if (!has_prefix)
        {
            if (const char* home = std::getenv("HOME"))
            {
                target.environment.push_back(std::format("WINEPREFIX={}/.wine", home));
            }
        }
        return target;
    }

    NameCache::NameCache(std::chrono::steady_clock::duration ttl) : ttl_(ttl) {}

    std::optional<std::string> NameCache::lookup(process::ProcessId pid, std::chrono::steady_clock::time_point now)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto                        entry = entries_.find(pid);
        if (entry == entries_.end())
        {
            return std::nullopt;
        }
        if (entry->second.expires <= now)
        {
            entries_.erase(entry);
            return std::nullopt;
        }
        return entry->second.name;
    }

    void NameCache::remember(process::ProcessId pid, std::string name, std::chrono::steady_clock::time_point now)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        sweep(now);
        entries_.insert_or_assign(pid, Entry {std::move(name), now + ttl_});
    }

    void NameCache::forget(process::ProcessId pid)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        entries_.erase(pid);
    }

    void NameCache::sweep(std::chrono::steady_clock::time_point now)
    {
        for (auto entry = entries_.begin(); entry != entries_.end();)
        {
            if (entry->second.expires <= now)
            {
                entry = entries_.erase(entry);
            }
            else
            {
                ++entry;
            }
        }

        while (entries_.size() >= kCapacity)
        {
            auto oldest = entries_.begin();
            for (auto entry = std::next(entries_.begin()); entry != entries_.end(); ++entry)
            {
                if (entry->second.expires < oldest->second.expires)
                {
                    oldest = entry;
                }
            }
            entries_.erase(oldest);
        }
    }

}
