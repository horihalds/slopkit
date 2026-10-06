// wine-proton: process access for games and software running under Wine or
// Steam Proton.
//
// Claims only processes detected as Wine/Proton, exposes their PE images as
// modules, and reads/writes memory through the same primitives as linux-proc.
// Its opt-in debug session uses ptrace only while an explicitly started debug
// session is open, exactly like linux-proc.
//
// The ABI plumbing (session set, string arena, error mapping, vtable and all 21
// entries) lives in the shared plugins/support library; this file is only the
// wine-proton profile plus the entry point.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "platform/linux/procfs.hpp"
#include "platform/linux/wine.hpp"
#include "plugins/support/plugin_support.hpp"

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
        // open; ordinary reads and writes stay on process_vm/procfs.
        return SLOPKIT_ACCESS_PROCESS_VM | SLOPKIT_ACCESS_PROCFS_MEM | SLOPKIT_ACCESS_PTRACE;
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

    // Wine exposes the target's PE images through the process maps; keep only
    // the PE ones so the module list is game-centric, then flag the main PE.
    void shape_modules(std::vector<slopkit::process::ModuleInfo>& modules, ProcessId pid)
    {
        std::erase_if(modules,
                      [](const slopkit::process::ModuleInfo& module)
                      {
                          return module.kind != slopkit::process::ModuleKind::pe;
                      });
        (void)slopkit::platform::flag_main_pe_image(pid, modules);
    }

    constexpr PluginProfile kProfile {
        info, precedence, access_methods, claim, shape_modules, slopkit::platform::ForeignSignalPolicy::forward};
} // namespace

extern "C" SLOPKIT_PLUGIN_EXPORT const slopkit_plugin_vtable* slopkit_plugin_entry(const slopkit_host_services* host)
{
    return slopkit::plugins::support::entry<kProfile>(host);
}
