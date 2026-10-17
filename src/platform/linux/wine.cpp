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
#include "platform/linux/module_entry.hpp"
#include "platform/linux/proc_text.hpp"
#include "platform/linux/procfs.hpp"

namespace slopkit::platform
{
    namespace
    {
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
    }

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

    std::optional<GuestImage> guest_image(process::ProcessId pid)
    {
        auto modules = modules_from_maps(read_maps(pid));
        if (!flag_main_pe_image(pid, modules))
        {
            return std::nullopt;
        }

        const auto* main = process::main_module(modules);
        if (main == nullptr || main->path.empty() || main->path.front() != '/')
        {
            return std::nullopt;
        }

        // Wine maps the guest's images from their host path, which the prefix's
        // mount namespace hides from this process. `/proc/<pid>/root` is the
        // target's own view and resolves either way.
        const auto machine = read_pe_machine(proc_entry(pid, "root" + main->path));
        if (!machine)
        {
            return std::nullopt;
        }

        const auto width = pe_pointer_size(*machine);
        if (!width)
        {
            return std::nullopt;
        }
        return GuestImage {main->path, *width};
    }

    std::optional<std::uint32_t> pe_pointer_size(std::uint32_t machine)
    {
        switch (machine)
        {
        case 0x8664: // IMAGE_FILE_MACHINE_AMD64
        case 0xAA64: // IMAGE_FILE_MACHINE_ARM64
            return 8;
        case 0x014C: // IMAGE_FILE_MACHINE_I386
        case 0x01C4: // IMAGE_FILE_MACHINE_ARMNT
            return 4;
        default:
            return std::nullopt;
        }
    }

    std::optional<std::string> guest_image_name(process::ProcessId pid)
    {
        const auto image = guest_image(pid);
        if (!image)
        {
            return std::nullopt;
        }
        return std::filesystem::path(image->path).filename().string();
    }

    std::uint32_t guest_pointer_size(process::ProcessId pid)
    {
        const auto image = guest_image(pid);
        return image ? image->pointer_size : 0;
    }

}
