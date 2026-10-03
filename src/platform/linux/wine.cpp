#include "platform/linux/wine.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "platform/linux/procfs.hpp"

namespace slopkit::platform
{

    namespace
    {
        std::string_view trim(std::string_view value)
        {
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0)
            {
                value.remove_prefix(1);
            }
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0)
            {
                value.remove_suffix(1);
            }
            return value;
        }

        std::vector<std::string_view> split_lines(std::string_view text)
        {
            std::vector<std::string_view> lines;
            std::size_t                   start = 0;
            while (start <= text.size())
            {
                const auto newline = text.find('\n', start);
                const auto length  = newline == std::string_view::npos ? std::string_view::npos : newline - start;
                lines.push_back(text.substr(start, length));
                if (newline == std::string_view::npos)
                {
                    break;
                }
                start = newline + 1;
            }
            return lines;
        }

        std::optional<std::string> read_file(const std::string& path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                return std::nullopt;
            }
            return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }

        std::string proc_entry(process::ProcessId pid, std::string_view name)
        {
            return "/proc/" + std::to_string(pid) + "/" + std::string(name);
        }

        // Lower-cased final path component, splitting both POSIX and Windows
        // separators so `C:\windows\winedevice.exe` reduces to `winedevice.exe`.
        std::string basename_lower(std::string_view path)
        {
            const auto  separator = path.find_last_of("/\\");
            const auto  name      = separator == std::string_view::npos ? path : path.substr(separator + 1);
            std::string result(name);
            std::ranges::transform(result,
                                   result.begin(),
                                   [](char character)
                                   {
                                       return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
                                   });
            return result;
        }

        std::string_view flavor_name(WineFlavor flavor)
        {
            switch (flavor)
            {
            case WineFlavor::proton:
                return "proton";
            case WineFlavor::wine:
                return "wine";
            case WineFlavor::none:
                break;
            }
            return "none";
        }
    } // namespace

    bool is_wine_mapped_path(std::string_view path)
    {
        if (path.empty())
        {
            return false;
        }
        if (path.find("/wine/") != std::string_view::npos)
        {
            return true;
        }
        if (path.find("drive_c") != std::string_view::npos)
        {
            return true;
        }
        if (path.find("/.wine/") != std::string_view::npos)
        {
            return true;
        }
        // Windows drive-letter path such as `Z:\game\game.exe`.
        return path.find(":\\") != std::string_view::npos;
    }

    WineClassification classify_wine_process(const std::vector<std::string>& environ,
                                             const std::vector<std::string>& cmdline,
                                             std::string_view                maps_text)
    {
        WineClassification result;
        bool               proton = false;

        const auto add_detail = [&result](std::string detail)
        {
            result.evidence.details.push_back(std::move(detail));
        };

        for (const auto& entry : environ)
        {
            const auto equals = entry.find('=');
            const auto key    = equals == std::string::npos ? entry : entry.substr(0, equals);

            if (key == "WINEPREFIX")
            {
                result.evidence.environ_signal = true;
                add_detail("env:WINEPREFIX");
            }
            else if (key == "STEAM_COMPAT_DATA_PATH" || key == "STEAM_COMPAT_CLIENT_INSTALL_PATH")
            {
                result.evidence.environ_signal = true;
                proton                         = true;
                add_detail("env:" + key);
            }
            else if (key.starts_with("PROTON"))
            {
                result.evidence.environ_signal = true;
                proton                         = true;
                add_detail("env:" + key);
            }
            else if (key.starts_with("WINE"))
            {
                result.evidence.environ_signal = true;
                add_detail("env:" + key);
            }
        }

        for (const auto& part : cmdline)
        {
            const auto name = basename_lower(part);

            if (name.starts_with("wineserver"))
            {
                result.evidence.cmdline_signal = true;
                add_detail("cmdline:wineserver");
            }
            else if (name.starts_with("wine") || name.ends_with(".exe"))
            {
                result.evidence.cmdline_signal = true;
                add_detail("cmdline:" + name);
            }

            if (name == "reaper" || part.find("pressure-vessel") != std::string::npos)
            {
                result.evidence.cmdline_signal = true;
                proton                         = true;
                add_detail("cmdline:proton-wrapper");
            }
        }

        for (const auto line : split_lines(maps_text))
        {
            const auto trimmed = trim(line);
            if (!is_wine_mapped_path(trimmed))
            {
                continue;
            }

            result.evidence.maps_signal = true;
            if (trimmed.find("/wine/") != std::string_view::npos)
            {
                add_detail("maps:/wine/");
            }
            else if (trimmed.find("drive_c") != std::string_view::npos)
            {
                add_detail("maps:drive_c");
            }
            else
            {
                add_detail("maps:wine-path");
            }
            break;
        }

        if (proton)
        {
            result.flavor = WineFlavor::proton;
        }
        else if (result.evidence.environ_signal || result.evidence.cmdline_signal || result.evidence.maps_signal)
        {
            result.flavor = WineFlavor::wine;
        }
        return result;
    }

    std::vector<std::string> read_environ(process::ProcessId pid)
    {
        const auto raw = read_file(proc_entry(pid, "environ"));
        if (!raw)
        {
            return {};
        }

        std::vector<std::string> entries;
        std::string              current;
        for (const char character : *raw)
        {
            if (character == '\0')
            {
                if (!current.empty())
                {
                    entries.push_back(current);
                    current.clear();
                }
                continue;
            }
            current.push_back(character);
        }
        if (!current.empty())
        {
            entries.push_back(current);
        }
        return entries;
    }

    std::string read_maps_text(process::ProcessId pid)
    {
        const auto raw = read_file(proc_entry(pid, "maps"));
        return raw ? *raw : std::string {};
    }

    WineClassification classify_wine_process(process::ProcessId pid)
    {
        const auto environ = read_environ(pid);
        const auto cmdline = read_cmdline_parts(pid);

        // Environment and command line are the cheapest signals; only fall back
        // to mapping the process when they say nothing.
        auto result = classify_wine_process(environ, cmdline, {});
        if (!result.claimed())
        {
            result = classify_wine_process(environ, cmdline, read_maps_text(pid));
        }

        if (result.claimed())
        {
            log::info(log::category::process, std::format("pid {} classified as {}", pid, flavor_name(result.flavor)));
            for (const auto& detail : result.evidence.details)
            {
                log::debug(log::category::process, std::format("pid {} wine signal: {}", pid, detail));
            }
        }
        return result;
    }

} // namespace slopkit::platform
