
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "platform/linux/procfs.hpp"
#include "platform/linux/wine.hpp"
#include "plugins/support/allocate.hpp"
#include "plugins/support/plugin_support.hpp"
#include "plugins/wine_proton/agent_service.hpp"

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
        out_name = process_name(pid);
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
        return slopkit::plugins::wine_proton::AgentService::modules(session);
    }

    std::expected<bool, slopkit::plugins::support::AccessFault>
    range_is_readable(slopkit::plugins::support::Session& session, std::uint64_t address, std::size_t size)
    {
        return slopkit::plugins::wine_proton::AgentService::validate(session, address, size);
    }

    std::expected<slopkit::plugins::support::MemoryResult, slopkit::plugins::support::AccessFault>
    read(slopkit::plugins::support::Session& session, std::uint64_t address, std::span<std::byte> buffer)
    {
        return slopkit::plugins::wine_proton::AgentService::read(session, address, buffer);
    }

    std::expected<slopkit::plugins::support::MemoryResult, slopkit::plugins::support::AccessFault>
    write(slopkit::plugins::support::Session& session, std::uint64_t address, std::span<const std::byte> data)
    {
        return slopkit::plugins::wine_proton::AgentService::write(session, address, data);
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
                                      range_is_readable,
                                      read,
                                      write,
                                      debug_excluded_threads};
}

extern "C" SLOPKIT_PLUGIN_EXPORT const slopkit_plugin_vtable* slopkit_plugin_entry(const slopkit_host_services* host)
{
    return slopkit::plugins::support::entry<kProfile>(host);
}
