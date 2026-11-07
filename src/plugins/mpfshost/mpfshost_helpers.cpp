#include "plugins/mpfshost/mpfshost_helpers.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <utility>

#include "plugin/plugin_api.h"

namespace slopkit::plugins::mpfshost
{
    namespace
    {
        std::string key_for(std::string_view target, std::uint32_t pid)
        {
            return std::format("{}\x1f{}", target, pid);
        }

        std::string lowercase(std::string_view text)
        {
            std::string result {text};
            std::ranges::transform(result,
                                   result.begin(),
                                   [](unsigned char character)
                                   {
                                       return static_cast<char>(std::tolower(character));
                                   });
            return result;
        }

        std::string_view basename(std::string_view path) noexcept
        {
            const auto separator = path.find_last_of("\\/");
            return separator == std::string_view::npos ? path : path.substr(separator + 1);
        }

        std::string stem(std::string value)
        {
            if (value.ends_with(".exe"))
            {
                value.resize(value.size() - 4);
            }
            return value;
        }
    }

    std::uint32_t TargetTable::remember(GuestTarget process)
    {
        const std::lock_guard lock(mutex_);
        const auto            key = key_for(process.target, process.pid);
        if (const auto found = by_key_.find(key); found != by_key_.end())
        {
            by_pid_[found->second] = std::move(process);
            return found->second;
        }

        const auto synthetic = next_++;
        by_key_.emplace(key, synthetic);
        by_pid_.emplace(synthetic, std::move(process));
        return synthetic;
    }

    std::optional<GuestTarget> TargetTable::lookup(std::uint32_t synthetic_pid) const
    {
        const std::lock_guard lock(mutex_);
        if (const auto found = by_pid_.find(synthetic_pid); found != by_pid_.end())
        {
            return found->second;
        }
        return std::nullopt;
    }

    RegionProtection classify_pte_flags(std::uint64_t flags) noexcept
    {
        constexpr std::uint64_t kNoExecute = 0x8000000000000000ull;
        constexpr std::uint64_t kWritable  = 0x2ull;
        return RegionProtection {
            .readable   = true,
            .writable   = (flags & kWritable) != 0,
            .executable = (flags & kNoExecute) == 0,
        };
    }

    bool is_main_module(std::string_view module_name, std::string_view process_name) noexcept
    {
        const auto module  = lowercase(basename(module_name));
        const auto process = lowercase(basename(process_name));
        if (module.empty() || process.empty())
        {
            return false;
        }
        return module == process || stem(module) == stem(process);
    }

    std::int32_t status_for_client_error(std::string_view error) noexcept
    {
        const auto text = lowercase(error);
        if (text.find("accessdenied") != std::string::npos || text.find("access denied") != std::string::npos
            || text.find("permission") != std::string::npos)
        {
            return SLOPKIT_ERR_PERMISSION_DENIED;
        }
        if (text.find("notfound") != std::string::npos || text.find("not found") != std::string::npos
            || text.find("no such") != std::string::npos || text.find("connect failed") != std::string::npos
            || text.find("targetnotfound") != std::string::npos)
        {
            return SLOPKIT_ERR_NOT_FOUND;
        }
        if (text.find("invalid") != std::string::npos || text.find("malformed") != std::string::npos)
        {
            return SLOPKIT_ERR_INVALID_ARGUMENT;
        }
        return SLOPKIT_ERR_IO;
    }

    std::string socket_path()
    {
        if (const char* value = std::getenv("SLOPKIT_MPFSHOST_SOCKET"); value != nullptr && value[0] != '\0')
        {
            return value;
        }
        return "/run/mpfshost.sock";
    }
}
