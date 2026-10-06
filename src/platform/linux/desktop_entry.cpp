#include "platform/linux/desktop_entry.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <ranges>
#include <system_error>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "platform/linux/proc_text.hpp"

namespace slopkit::platform
{

    std::optional<DesktopEntry> parse_desktop_entry(std::string_view text)
    {
        bool         in_group    = false;
        bool         found_group = false;
        DesktopEntry entry;

        std::size_t position = 0;
        while (position <= text.size())
        {
            const auto newline = text.find('\n', position);
            const auto line    = trim(
                text.substr(position, newline == std::string_view::npos ? std::string_view::npos : newline - position));
            if (newline == std::string_view::npos)
            {
                position = text.size() + 1;
            }
            else
            {
                position = newline + 1;
            }

            if (line.empty() || line.front() == '#')
            {
                continue;
            }

            if (line.front() == '[')
            {
                if (in_group)
                {
                    break;
                }
                const auto close = line.find(']');
                if (close != std::string_view::npos && line.substr(1, close - 1) == "Desktop Entry")
                {
                    in_group    = true;
                    found_group = true;
                }
                continue;
            }

            if (!in_group)
            {
                continue;
            }

            const auto equals = line.find('=');
            if (equals == std::string_view::npos)
            {
                continue;
            }
            const std::string_view key   = trim(line.substr(0, equals));
            const std::string_view value = trim(line.substr(equals + 1));

            if (key == "Name" && entry.name.empty())
            {
                entry.name = value;
            }
            else if (key == "Exec")
            {
                entry.exec = value;
            }
            else if (key == "Type")
            {
                entry.type = value;
            }
            else if (key == "NoDisplay")
            {
                entry.no_display = value == "true";
            }
            else if (key == "Hidden")
            {
                entry.hidden = value == "true";
            }
        }

        if (!found_group)
        {
            return std::nullopt;
        }
        return entry;
    }

    std::optional<std::string> desktop_exec_basename(std::string_view exec)
    {
        exec = trim(exec);
        if (exec.empty())
        {
            return std::nullopt;
        }

        std::string token;
        if (exec.front() == '"')
        {
            const auto quote = exec.find('"', 1);
            token = std::string(quote == std::string_view::npos ? exec.substr(1) : exec.substr(1, quote - 1));
        }
        else
        {
            const auto space = exec.find_first_of(" \t");
            token            = std::string(space == std::string_view::npos ? exec : exec.substr(0, space));
        }

        // Strip a trailing field code such as `%u` or `%F`.
        if (const auto percent = token.find('%'); percent != std::string::npos)
        {
            token.erase(percent);
        }
        if (token.empty())
        {
            return std::nullopt;
        }

        const auto slash = token.find_last_of('/');
        return slash == std::string::npos ? token : token.substr(slash + 1);
    }

    std::vector<std::filesystem::path> default_application_dirs()
    {
        std::vector<std::filesystem::path> dirs;
        auto                               add = [&dirs](std::string_view base)
        {
            if (!base.empty())
            {
                dirs.emplace_back(std::filesystem::path(std::string(base)) / "applications");
            }
        };

        if (const char* data_home = std::getenv("XDG_DATA_HOME"); data_home != nullptr && *data_home != '\0')
        {
            add(data_home);
        }
        else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
        {
            add(std::string(home) + "/.local/share");
        }

        std::string data_dirs = "/usr/local/share:/usr/share";
        if (const char* env = std::getenv("XDG_DATA_DIRS"); env != nullptr && *env != '\0')
        {
            data_dirs = env;
        }

        std::size_t start = 0;
        while (start <= data_dirs.size())
        {
            const auto separator = data_dirs.find(':', start);
            add(std::string_view(data_dirs).substr(
                start, separator == std::string::npos ? std::string::npos : separator - start));
            if (separator == std::string::npos)
            {
                break;
            }
            start = separator + 1;
        }

        return dirs;
    }

    std::vector<std::string> scan_desktop_executables(std::span<const std::filesystem::path> application_dirs)
    {
        log::info(log::category::process,
                  std::format("desktop entry index: scanning {} director(ies)", application_dirs.size()));

        std::vector<std::string> executables;
        for (const auto& dir : application_dirs)
        {
            std::error_code error;
            if (!std::filesystem::is_directory(dir, error))
            {
                log::debug(log::category::process,
                           std::format("desktop entry index: {} is not a directory", dir.string()));
                continue;
            }
            for (std::filesystem::recursive_directory_iterator it(dir, error), end; it != end; it.increment(error))
            {
                if (error)
                {
                    break;
                }
                if (!it->is_regular_file(error) || it->path().extension() != ".desktop")
                {
                    continue;
                }

                std::ifstream file(it->path(), std::ios::binary);
                if (!file)
                {
                    continue;
                }
                const std::string content {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
                const auto        entry = parse_desktop_entry(content);
                if (!entry)
                {
                    log::warning(log::category::process,
                                 std::format("desktop entry index: malformed {}", it->path().string()));
                    continue;
                }
                if (entry->type != "Application" || entry->no_display || entry->hidden)
                {
                    continue;
                }
                if (const auto base = desktop_exec_basename(entry->exec))
                {
                    executables.push_back(*base);
                }
            }
        }

        std::ranges::sort(executables);
        executables.erase(std::ranges::unique(executables).begin(), executables.end());
        log::info(log::category::process, std::format("desktop entry index: {} executable(s)", executables.size()));
        return executables;
    }

    bool is_desktop_application(std::string_view exe_path, std::span<const std::string> executables)
    {
        if (exe_path.empty() || executables.empty())
        {
            return false;
        }
        const auto slash = exe_path.find_last_of('/');
        const auto base  = slash == std::string_view::npos ? exe_path : exe_path.substr(slash + 1);
        return std::ranges::find(executables, base) != executables.end();
    }

} // namespace slopkit::platform
