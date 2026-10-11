// wine-proton: process access for games and software running under Wine or
// Steam Proton.
//
// Claims only processes detected as Wine/Proton, exposes their PE images as
// modules, and reads/writes memory through the same primitives as linux-proc.
// Its opt-in debug session uses ptrace only while an explicitly started debug
// session is open, exactly like linux-proc.
//
// The ABI plumbing (session set, string arena, error mapping, vtable and all 23
// entries) lives in the shared plugins/support library; this file is only the
// wine-proton profile plus the entry point.
//
// The module list comes from the target's own loader list through the resident
// agent, not from the host's reading of the process maps.

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
        // Lower than linux-proc, so it is the default for a Wine process both
        // plugins can see.
        return 10;
    }

    uint32_t access_methods() noexcept
    {
        // PTRACE only ever runs while an explicitly started debug session is
        // open; ordinary reads and writes ride the resident agent while the
        // target can run, and process_vm/procfs while a debug session stops it.
        return SLOPKIT_ACCESS_PROCESS_VM | SLOPKIT_ACCESS_PROCFS_MEM | SLOPKIT_ACCESS_PTRACE | SLOPKIT_ACCESS_AGENT;
    }

    // The name the picker shows: /proc/<pid>/status first, then the exe filename.
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

    // Claims only the processes Wine/Proton owns.
    bool claim(ProcessId pid, std::string& out_name)
    {
        if (!slopkit::platform::classify_wine_process(pid).claimed())
        {
            return false;
        }
        out_name = process_name(pid);
        return true;
    }

    // Allocation goes through the resident agent alone, so the mapping is one
    // Wine's own virtual-memory manager created; there is no ptrace fallback and
    // the log names the agent that served the call.
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

    // Closing the session SHUTDOWNs the agent (which frees what it allocated),
    // unmaps the shared region and removes the per-session directory.
    void close_session(slopkit::plugins::support::Session& session)
    {
        slopkit::plugins::wine_proton::AgentService::close(session.pid);
    }

    // The region list comes from the target's own VirtualQuery walk, merged with
    // the procfs paths so module attribution survives.
    std::expected<std::vector<slopkit::platform::MappedRegion>, slopkit::plugins::support::AccessFault>
    regions(slopkit::plugins::support::Session& session)
    {
        return slopkit::plugins::wine_proton::AgentService::regions(session);
    }

    // The module list comes from the target's own loader list, merged with the
    // procfs paths. Agent-only, so while the target cannot run the reason is
    // returned instead of a host-derived list.
    std::expected<std::vector<slopkit::process::ModuleInfo>, slopkit::plugins::support::AccessFault>
    modules(slopkit::plugins::support::Session& session)
    {
        return slopkit::plugins::wine_proton::AgentService::modules(session);
    }

    // Address validation answers from the target's own commit and protection
    // view while it can run, and from the shared read probe otherwise.
    std::expected<bool, slopkit::plugins::support::AccessFault>
    range_is_readable(slopkit::plugins::support::Session& session, std::uint64_t address, std::size_t size)
    {
        return slopkit::plugins::wine_proton::AgentService::validate(session, address, size);
    }

    // Reads and writes ride the resident agent while the target can run, and the
    // host session memory while a debug session has it stopped.
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
                                      write};
} // namespace

extern "C" SLOPKIT_PLUGIN_EXPORT const slopkit_plugin_vtable* slopkit_plugin_entry(const slopkit_host_services* host)
{
    return slopkit::plugins::support::entry<kProfile>(host);
}
