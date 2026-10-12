
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "platform/linux/procfs.hpp"
#include "platform/linux/wine.hpp"
#include "plugins/support/allocate.hpp"
#include "plugins/support/plugin_support.hpp"
#include "plugins/wine_proton/agent_service.hpp"
#include "plugins/wine_proton/wine_target.hpp"

namespace
{
    using slopkit::plugins::support::PluginProfile;
    using slopkit::process::ProcessId;

    const slopkit_plugin_info kInfo {
        "wine-proton",
        "Wine/Proton process access",
        "0.1.0",
        "Claims Wine and Steam Proton processes and exposes their PE images; the opt-in debugger uses ptrace."};

    const slopkit_plugin_info* info() noexcept
    {
        return &kInfo;
    }

    int32_t precedence() noexcept
    {
        return 10;
    }

    uint32_t access_methods() noexcept
    {
        return SLOPKIT_ACCESS_PROCESS_VM | SLOPKIT_ACCESS_PROCFS_MEM | SLOPKIT_ACCESS_PTRACE | SLOPKIT_ACCESS_AGENT;
    }

    std::string process_name(ProcessId pid)
    {
        if (const auto status = slopkit::platform::read_status(pid))
        {
            if (!status->name.empty())
            {
                return status->name;
            }
        }
        if (const auto exe = slopkit::platform::read_exe(pid))
        {
            return std::filesystem::path(*exe).filename().string();
        }
        return {};
    }

    bool claim(ProcessId pid, std::string& out_name)
    {
        if (!slopkit::platform::classify_wine_process(pid).claimed())
        {
            return false;
        }

        static slopkit::plugins::wine_proton::NameCache cache;
        const auto                                      now = std::chrono::steady_clock::now();
        if (const auto remembered = cache.lookup(pid, now))
        {
            out_name = *remembered;
            return true;
        }

        out_name = slopkit::plugins::wine_proton::guest_image_name(pid).value_or(process_name(pid));
        cache.remember(pid, out_name, now);
        return true;
    }

    std::expected<std::uint64_t, slopkit::plugins::support::AllocationError>
    allocate(slopkit::plugins::support::Session& session, std::uint64_t size, std::uint64_t near_address)
    {
        return slopkit::plugins::wine_proton::AgentService::allocate(session, size, near_address);
    }

    std::expected<void, slopkit::plugins::support::AllocationError> free(slopkit::plugins::support::Session& session,
                                                                         std::uint64_t                       address)
    {
        return slopkit::plugins::wine_proton::AgentService::free(session, address);
    }

    void close_session(slopkit::plugins::support::Session& session)
    {
        slopkit::plugins::wine_proton::AgentService::close(session.pid);
    }

    std::expected<std::vector<slopkit::platform::MappedRegion>, slopkit::plugins::support::AccessFault>
    regions(slopkit::plugins::support::Session& session)
    {
        return slopkit::plugins::wine_proton::AgentService::regions(session);
    }

    std::expected<std::vector<slopkit::process::ModuleInfo>, slopkit::plugins::support::AccessFault>
    modules(slopkit::plugins::support::Session& session)
    {
        auto result = slopkit::plugins::wine_proton::AgentService::modules(session);
        if (!result)
        {
            return result;
        }
        // Every PE module of a Wine target shares the guest's bitness, which is
        // what the host needs to decode the module's code in the right mode.
        const std::uint32_t pointer_size = slopkit::plugins::wine_proton::guest_pointer_size(session.pid);
        for (auto& module : *result)
        {
            module.pointer_size = pointer_size;
        }
        return result;
    }

    std::vector<ProcessId> debug_excluded_threads(slopkit::plugins::support::Session& session)
    {
        return slopkit::plugins::wine_proton::AgentService::agent_threads(session);
    }

    constexpr PluginProfile kProfile {info,
                                      precedence,
                                      access_methods,
                                      claim,
                                      nullptr,
                                      modules,
                                      slopkit::platform::ForeignSignalPolicy::forward,
                                      allocate,
                                      free,
                                      close_session,
                                      regions,
                                      nullptr,
                                      nullptr,
                                      nullptr,
                                      debug_excluded_threads};
}

extern "C" SLOPKIT_PLUGIN_EXPORT const slopkit_plugin_vtable* slopkit_plugin_entry(const slopkit_host_services* host)
{
    return slopkit::plugins::support::entry<kProfile>(host);
}
