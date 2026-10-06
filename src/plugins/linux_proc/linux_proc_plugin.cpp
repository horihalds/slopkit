// linux-proc: process access for any normal Linux process.
//
// Enumerates /proc, resolves module regions and threads, and reads/writes memory
// through process_vm_readv/process_vm_writev with a /proc/<pid>/mem fallback.
// The read/write path never calls ptrace, so nothing like TracerPid appears in
// the target. The opt-in debugger operations (ABI 1.4) do use ptrace, but only
// while an explicitly started debug session is open.
//
// The ABI plumbing (session set, string arena, error mapping, vtable and all 21
// entries) lives in the shared plugins/support library; this file is only the
// linux-proc profile plus the entry point.

#include <cstdint>
#include <string>
#include <vector>

#include "platform/linux/procfs.hpp"
#include "plugins/support/plugin_support.hpp"

namespace
{
    using slopkit::plugins::support::PluginProfile;
    using slopkit::process::ProcessId;

    const slopkit_plugin_info kInfo {
        "linux-proc",
        "Linux process access",
        "0.2.0",
        "Enumerates any accessible Linux process, reads/writes its memory without ptrace and can run an "
        "opt-in ptrace debug session."};

    const slopkit_plugin_info* info() noexcept
    {
        return &kInfo;
    }

    std::int32_t precedence() noexcept
    {
        return 100;
    }

    std::uint32_t access_methods() noexcept
    {
        // ptrace is advertised because the plugin can run the opt-in debugger;
        // the read/write path still reports process_vm/procfs per session.
        return SLOPKIT_ACCESS_PROCESS_VM | SLOPKIT_ACCESS_PROCFS_MEM | SLOPKIT_ACCESS_PTRACE;
    }

    // Claims every pid and shows the process name from /proc/<pid>/status.
    bool claim(ProcessId pid, std::string& out_name)
    {
        const auto status = slopkit::platform::read_status(pid);
        out_name          = status ? status->name : std::string {};
        return true;
    }

    // Flags the exe-backed module as the main image; every module stays listed.
    void shape_modules(std::vector<slopkit::process::ModuleInfo>& modules, ProcessId pid)
    {
        if (const auto exe = slopkit::platform::read_exe(pid))
        {
            (void)slopkit::platform::flag_main_module_by_path(modules, *exe);
        }
    }

    constexpr PluginProfile kProfile {
        info, precedence, access_methods, claim, shape_modules, slopkit::platform::ForeignSignalPolicy::suppress};
} // namespace

extern "C" SLOPKIT_PLUGIN_EXPORT const slopkit_plugin_vtable* slopkit_plugin_entry(const slopkit_host_services* host)
{
    return slopkit::plugins::support::entry<kProfile>(host);
}
