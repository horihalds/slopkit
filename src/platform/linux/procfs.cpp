#include "platform/linux/procfs.hpp"

#include "platform/linux/module_entry.hpp"
#include "platform/linux/proc_text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <unistd.h>

#include "core/log.hpp"
#include "core/log_categories.hpp"

namespace slopkit::platform
{

    namespace
    {
        std::string_view next_token(std::string_view line, std::size_t& cursor)
        {
            while (cursor < line.size() && line[cursor] == ' ')
            {
                ++cursor;
            }
            const auto start = cursor;
            while (cursor < line.size() && line[cursor] != ' ')
            {
                ++cursor;
            }
            return line.substr(start, cursor - start);
        }

        std::string_view first_token(std::string_view value)
        {
            std::size_t cursor = 0;
            return next_token(value, cursor);
        }

        std::uint64_t to_uint(std::string_view value)
        {
            std::uint64_t result = 0;
            std::from_chars(value.data(), value.data() + value.size(), result);
            return result;
        }

        std::uint64_t from_hex(std::string_view value)
        {
            std::uint64_t result = 0;
            std::from_chars(value.data(), value.data() + value.size(), result, 16);
            return result;
        }

        std::string lowercase(std::string_view value)
        {
            std::string result(value);
            std::ranges::transform(result,
                                   result.begin(),
                                   [](char character)
                                   {
                                       return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
                                   });
            return result;
        }
    } // namespace

    std::vector<process::ProcessId> list_pids()
    {
        std::vector<process::ProcessId> pids;
        std::error_code                 error;
        for (const auto& entry : std::filesystem::directory_iterator("/proc", error))
        {
            if (error)
            {
                break;
            }
            const auto name = entry.path().filename().string();
            if (!is_all_digits(name))
            {
                continue;
            }
            pids.push_back(static_cast<process::ProcessId>(to_uint(name)));
        }
        if (error)
        {
            log::warning(log::category::process, std::format("cannot enumerate /proc: {}", error.message()));
        }
        else
        {
            log::debug(log::category::process, std::format("enumerated {} process(es)", pids.size()));
        }
        std::ranges::sort(pids);
        return pids;
    }

    std::optional<std::string> read_exe(process::ProcessId pid)
    {
        std::array<char, 4096> buffer {};
        const auto             path   = proc_entry(pid, "exe");
        const auto             length = ::readlink(path.c_str(), buffer.data(), buffer.size() - 1);
        if (length <= 0)
        {
            return std::nullopt;
        }
        return std::string(buffer.data(), static_cast<std::size_t>(length));
    }

    std::vector<std::string> read_cmdline_parts(process::ProcessId pid)
    {
        const auto raw = read_file(proc_entry(pid, "cmdline"));
        if (!raw)
        {
            return {};
        }

        std::vector<std::string> parts;
        std::string              current;
        for (const char character : *raw)
        {
            if (character == '\0')
            {
                if (!current.empty())
                {
                    parts.push_back(current);
                    current.clear();
                }
                continue;
            }
            current.push_back(character);
        }
        if (!current.empty())
        {
            parts.push_back(current);
        }
        return parts;
    }

    std::optional<std::string> read_cmdline(process::ProcessId pid)
    {
        const auto raw = read_file(proc_entry(pid, "cmdline"));
        if (!raw)
        {
            return std::nullopt;
        }

        std::string result;
        result.reserve(raw->size());
        for (const char character : *raw)
        {
            result.push_back(character == '\0' ? ' ' : character);
        }
        return std::string(trim(result));
    }

    std::optional<ProcessStatus> read_status(process::ProcessId pid)
    {
        const auto raw = read_file(proc_entry(pid, "status"));
        if (!raw)
        {
            return std::nullopt;
        }
        return parse_status(*raw);
    }

    std::optional<ProcessStatus> parse_status(std::string_view text)
    {
        ProcessStatus status;
        bool          found_name = false;

        for (const auto line : split_lines(text))
        {
            const auto colon = line.find(':');
            if (colon == std::string_view::npos)
            {
                continue;
            }
            const auto key   = trim(line.substr(0, colon));
            const auto value = trim(line.substr(colon + 1));

            if (key == "Name")
            {
                status.name = std::string(value);
                found_name  = true;
            }
            else if (key == "PPid")
            {
                status.ppid = static_cast<std::uint32_t>(to_uint(value));
            }
            else if (key == "TracerPid")
            {
                status.tracer_pid = static_cast<int>(to_uint(value));
            }
            else if (key == "Uid")
            {
                status.uid = static_cast<std::uint32_t>(to_uint(first_token(value)));
            }
            else if (key == "Gid")
            {
                status.gid = static_cast<std::uint32_t>(to_uint(first_token(value)));
            }
        }

        if (!found_name)
        {
            return std::nullopt;
        }
        return status;
    }

    std::vector<MappedRegion> read_maps(process::ProcessId pid)
    {
        const auto raw = read_file(proc_entry(pid, "maps"));
        if (!raw)
        {
            log::warning(log::category::process, std::format("cannot read the memory map of pid {}", pid));
            return {};
        }
        return parse_maps(*raw);
    }

    std::vector<MappedRegion> parse_maps(std::string_view text)
    {
        constexpr std::string_view deleted_suffix = " (deleted)";

        std::vector<MappedRegion> regions;
        for (auto line : split_lines(text))
        {
            line = trim(line);
            if (line.empty())
            {
                continue;
            }

            std::size_t cursor = 0;
            const auto  range  = next_token(line, cursor);
            const auto  dash   = range.find('-');
            if (dash == std::string_view::npos)
            {
                continue;
            }

            MappedRegion region;
            region.start = from_hex(range.substr(0, dash));
            region.end   = from_hex(range.substr(dash + 1));

            const auto permissions = next_token(line, cursor);
            if (permissions.size() >= 4)
            {
                region.readable   = permissions[0] == 'r';
                region.writable   = permissions[1] == 'w';
                region.executable = permissions[2] == 'x';
                region.shared     = permissions[3] == 's';
            }

            region.offset = from_hex(next_token(line, cursor));
            (void)next_token(line, cursor); // device
            (void)next_token(line, cursor); // inode

            while (cursor < line.size() && line[cursor] == ' ')
            {
                ++cursor;
            }
            std::string path(line.substr(cursor));
            if (path.size() >= deleted_suffix.size() && path.ends_with(deleted_suffix))
            {
                path.resize(path.size() - deleted_suffix.size());
                region.deleted = true;
            }
            region.path = std::move(path);
            regions.push_back(std::move(region));
        }
        return regions;
    }

    std::vector<std::uint32_t> parse_task_ids(std::string_view text)
    {
        std::vector<std::uint32_t> tids;
        for (const auto line : split_lines(text))
        {
            const auto value = trim(line);
            if (is_all_digits(value))
            {
                tids.push_back(static_cast<std::uint32_t>(to_uint(value)));
            }
        }
        std::ranges::sort(tids);
        return tids;
    }

    std::vector<process::ThreadInfo> read_threads(process::ProcessId pid)
    {
        std::vector<process::ThreadInfo> threads;
        const auto                       directory = proc_entry(pid, "task");

        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(directory, error))
        {
            if (error)
            {
                break;
            }
            const auto name = entry.path().filename().string();
            if (!is_all_digits(name))
            {
                continue;
            }

            process::ThreadInfo thread;
            thread.tid = static_cast<std::uint32_t>(to_uint(name));
            if (const auto comm = read_file(entry.path() / "comm"))
            {
                thread.name = std::string(trim(*comm));
            }
            threads.push_back(std::move(thread));
        }

        if (error)
        {
            log::warning(log::category::process,
                         std::format("cannot enumerate the threads of pid {}: {}", pid, error.message()));
        }
        else
        {
            log::debug(log::category::process, std::format("pid {} has {} thread(s)", pid, threads.size()));
        }

        std::ranges::sort(threads, {}, &process::ThreadInfo::tid);
        return threads;
    }

    std::optional<char> read_thread_state(process::ProcessId pid, process::ProcessId tid)
    {
        const auto raw = read_file(proc_entry(pid, "task/" + std::to_string(tid) + "/stat"));
        if (!raw)
        {
            return std::nullopt;
        }

        // A thread's name (the second field) may contain spaces and parentheses,
        // so the state is the first field after the last ')'.
        const auto close = raw->rfind(')');
        if (close == std::string::npos)
        {
            return std::nullopt;
        }
        const auto rest = trim(std::string_view(*raw).substr(close + 1));
        if (rest.empty())
        {
            return std::nullopt;
        }
        return rest.front();
    }

    std::optional<std::int64_t> read_thread_syscall(process::ProcessId pid, process::ProcessId tid)
    {
        const auto raw = read_file(proc_entry(pid, "task/" + std::to_string(tid) + "/syscall"));
        if (!raw)
        {
            return std::nullopt;
        }

        // The first field is the syscall number while the thread is inside a
        // call and -1 while it is not; "running" and any other word say the
        // same thing, so both are reported as -1.
        const auto field = first_token(trim(*raw));
        if (field.empty())
        {
            return std::nullopt;
        }
        if (field == "running")
        {
            return std::int64_t {-1};
        }

        std::int64_t number = 0;
        const auto   parsed = std::from_chars(field.data(), field.data() + field.size(), number);
        if (parsed.ec != std::errc {} || parsed.ptr != field.data() + field.size())
        {
            return std::nullopt;
        }
        return number;
    }

    std::string read_thread_wchan(process::ProcessId pid, process::ProcessId tid)
    {
        const auto raw = read_file(proc_entry(pid, "task/" + std::to_string(tid) + "/wchan"));
        return raw ? std::string(trim(*raw)) : std::string {};
    }

    process::ModuleKind classify_region(const MappedRegion& region)
    {
        if (region.path.empty() || region.path.front() == '[')
        {
            return process::ModuleKind::anonymous;
        }

        const auto path = lowercase(region.path);
        if (path.ends_with(".exe") || path.ends_with(".dll") || path.ends_with(".sys"))
        {
            return process::ModuleKind::pe;
        }
        if (path.find("drive_c") != std::string::npos)
        {
            return process::ModuleKind::pe;
        }
        if (region.path.size() > 2 && std::isalpha(static_cast<unsigned char>(region.path[0])) != 0
            && region.path[1] == ':')
        {
            return process::ModuleKind::pe;
        }
        return process::ModuleKind::elf;
    }

    std::string region_name(const MappedRegion& region)
    {
        if (classify_region(region) == process::ModuleKind::anonymous)
        {
            return "[anon]";
        }
        return std::filesystem::path(region.path).filename().string();
    }

    std::vector<process::ModuleInfo> modules_from_maps(const std::vector<MappedRegion>& regions)
    {
        std::vector<process::ModuleInfo>   modules;
        std::map<std::string, std::size_t> module_by_path;

        for (const auto& region : regions)
        {
            const auto kind = classify_region(region);
            if (kind == process::ModuleKind::anonymous)
            {
                process::ModuleInfo module;
                module.base   = region.start;
                module.size   = region.end - region.start;
                module.offset = region.offset;
                module.kind   = process::ModuleKind::anonymous;
                module.name   = region_name(region);
                modules.push_back(std::move(module));
                continue;
            }

            const auto found = module_by_path.find(region.path);
            if (found == module_by_path.end())
            {
                process::ModuleInfo module;
                module.base   = region.start;
                module.size   = region.end - region.start;
                module.offset = region.offset;
                module.kind   = kind;
                module.path   = region.path;
                module.name   = region_name(region);
                module_by_path.emplace(region.path, modules.size());
                modules.push_back(std::move(module));
                continue;
            }

            auto&      module    = modules[found->second];
            const auto new_start = std::min(module.base, region.start);
            const auto new_end   = std::max(module.base + module.size, region.end);
            module.base          = new_start;
            module.size          = new_end - new_start;
        }

        std::ranges::sort(modules, {}, &process::ModuleInfo::base);
        return modules;
    }

    void fill_module_entry_points(process::ProcessId pid, std::vector<process::ModuleInfo>& modules)
    {
        const auto root = std::filesystem::path("/proc") / std::to_string(pid) / "root";

        for (auto& module : modules)
        {
            // Only a module whose first mapping starts the file has a readable,
            // unambiguous image header at its base.
            if (module.kind == process::ModuleKind::anonymous || module.path.empty() || module.offset != 0)
            {
                continue;
            }

            const std::filesystem::path path(module.path);
            if (!path.is_absolute())
            {
                continue;
            }

            const auto image = read_image_entry(root / path.relative_path());
            if (!image)
            {
                continue;
            }
            module.entry = image->address + (image->relative ? module.base : 0);
        }
    }

    bool flag_main_module_by_path(std::vector<process::ModuleInfo>& modules, std::string_view exe_path)
    {
        for (auto& module : modules)
        {
            module.is_main = false;
        }
        if (exe_path.empty())
        {
            return false;
        }

        constexpr std::string_view kDeletedSuffix = " (deleted)";
        const bool                 has_deleted    = exe_path.ends_with(kDeletedSuffix);
        const std::string_view     stripped =
            has_deleted ? exe_path.substr(0, exe_path.size() - kDeletedSuffix.size()) : exe_path;

        for (auto& module : modules)
        {
            if (module.path.empty() || !process::is_file_backed(module))
            {
                continue;
            }
            if (module.path == exe_path || (has_deleted && module.path == stripped))
            {
                module.is_main = true;
                return true;
            }
        }
        return false;
    }

    bool flag_main_pe_image(process::ProcessId pid, std::vector<process::ModuleInfo>& modules)
    {
        for (auto& module : modules)
        {
            module.is_main = false;
        }

        const auto root = std::filesystem::path("/proc") / std::to_string(pid) / "root";

        process::ModuleInfo* chosen = nullptr;
        for (auto& module : modules)
        {
            if (module.kind != process::ModuleKind::pe || module.path.empty() || module.offset != 0)
            {
                continue;
            }

            const std::filesystem::path path(module.path);
            if (!path.is_absolute())
            {
                continue;
            }

            if (read_pe_kind(root / path.relative_path()) != PeKind::executable)
            {
                continue;
            }
            if (chosen == nullptr || module.base < chosen->base)
            {
                chosen = &module;
            }
        }

        if (chosen == nullptr)
        {
            return false;
        }
        chosen->is_main = true;
        return true;
    }

} // namespace slopkit::platform
