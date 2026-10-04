#include "process/types.hpp"

#include <cctype>
#include <cstddef>

namespace slopkit::process
{

    namespace
    {
        std::string_view basename(std::string_view path) noexcept
        {
            const auto slash = path.find_last_of('/');
            return slash == std::string_view::npos ? path : path.substr(slash + 1);
        }

        bool equals_ignore_case(std::string_view lhs, std::string_view rhs) noexcept
        {
            if (lhs.size() != rhs.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < lhs.size(); ++i)
            {
                const auto a = static_cast<unsigned char>(lhs[i]);
                const auto b = static_cast<unsigned char>(rhs[i]);
                if (std::tolower(a) != std::tolower(b))
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    const ModuleInfo* main_module(std::span<const ModuleInfo> modules) noexcept
    {
        const ModuleInfo* lowest = nullptr;
        for (const ModuleInfo& module : modules)
        {
            if (!is_file_backed(module))
            {
                continue;
            }
            if (module.is_main)
            {
                return &module;
            }
            if (lowest == nullptr || module.base < lowest->base)
            {
                lowest = &module;
            }
        }
        return lowest;
    }

    const ProcessInfo*
    match_process_by_name(std::span<const ProcessInfo> processes, std::string_view name, bool match_exe_path) noexcept
    {
        if (name.empty())
        {
            return nullptr;
        }

        const ProcessInfo* best = nullptr;
        for (const ProcessInfo& info : processes)
        {
            const bool matches = equals_ignore_case(info.name, name)
                              || (match_exe_path && equals_ignore_case(basename(info.exe_path), name));
            if (!matches)
            {
                continue;
            }
            if (best == nullptr || info.pid < best->pid)
            {
                best = &info;
            }
        }
        return best;
    }

    const ProcessInfo* match_process_by_exe_path(std::span<const ProcessInfo> processes,
                                                 std::string_view             exe_path) noexcept
    {
        if (exe_path.empty())
        {
            return nullptr;
        }

        const std::string_view wanted = basename(exe_path);
        if (wanted.empty())
        {
            return nullptr;
        }

        const ProcessInfo* best = nullptr;
        for (const ProcessInfo& info : processes)
        {
            if (!equals_ignore_case(basename(info.exe_path), wanted))
            {
                continue;
            }
            if (best == nullptr || info.pid < best->pid)
            {
                best = &info;
            }
        }
        return best;
    }

    std::string_view describe(AccessError error) noexcept
    {
        switch (error)
        {
        case AccessError::permission_denied:
            return "permission denied";
        case AccessError::not_found:
            return "process or object not found";
        case AccessError::unsupported:
            return "unsupported operation";
        case AccessError::io_error:
            return "I/O error";
        case AccessError::invalid_abi:
            return "invalid plugin ABI";
        case AccessError::invalid_argument:
            return "invalid argument";
        case AccessError::internal:
            return "internal error";
        }
        return "unknown error";
    }

    std::string_view describe(ModuleKind kind) noexcept
    {
        switch (kind)
        {
        case ModuleKind::elf:
            return "ELF";
        case ModuleKind::pe:
            return "PE";
        case ModuleKind::anonymous:
            return "anonymous";
        }
        return "unknown";
    }

    std::string describe(AccessMethod methods)
    {
        if (methods == AccessMethod::none)
        {
            return "none";
        }

        std::string result;
        const auto  append = [&result](std::string_view name)
        {
            if (!result.empty())
            {
                result += ", ";
            }
            result += name;
        };

        if (has_flag(methods, AccessMethod::procfs_mem))
        {
            append("procfs");
        }
        if (has_flag(methods, AccessMethod::process_vm))
        {
            append("process_vm");
        }
        if (has_flag(methods, AccessMethod::ptrace))
        {
            append("ptrace");
        }
        if (has_flag(methods, AccessMethod::wineserver))
        {
            append("wineserver");
        }
        return result;
    }

} // namespace slopkit::process
