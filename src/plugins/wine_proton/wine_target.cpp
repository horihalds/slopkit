#include "plugins/wine_proton/wine_target.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <optional>
#include <ranges>
#include <string_view>

#include <unistd.h>

#include "platform/linux/module_entry.hpp"
#include "platform/linux/procfs.hpp"
#include "platform/linux/wine.hpp"

namespace slopkit::plugins::wine_proton
{
    namespace
    {
        struct GuestImage
        {
            std::string   path;
            std::uint32_t pointer_size {0};
        };

        std::optional<GuestImage> guest_image(process::ProcessId pid)
        {
            auto modules = platform::modules_from_maps(platform::read_maps(pid));
            if (!platform::flag_main_pe_image(pid, modules))
            {
                return std::nullopt;
            }
            const auto* main = process::main_module(modules);
            if (main == nullptr || main->path.empty() || main->path.front() != '/')
            {
                return std::nullopt;
            }

            const std::filesystem::path path(main->path);
            const auto                  root    = std::filesystem::path("/proc") / std::to_string(pid) / "root";
            const auto                  machine = platform::read_pe_machine(root / path.relative_path());
            if (!machine)
            {
                return std::nullopt;
            }

            switch (*machine)
            {
            case 0x8664:
            case 0xAA64:
                return GuestImage {main->path, 8};
            case 0x014C:
            case 0x01C4:
                return GuestImage {main->path, 4};
            default:
                return std::nullopt;
            }
        }

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
        const auto image = guest_image(pid);
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

}
