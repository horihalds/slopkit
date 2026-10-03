#include "process/types.hpp"

namespace slopkit::process
{

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
