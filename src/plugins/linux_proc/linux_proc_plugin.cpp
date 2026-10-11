
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
        return SLOPKIT_ACCESS_PROCESS_VM | SLOPKIT_ACCESS_PROCFS_MEM | SLOPKIT_ACCESS_PTRACE;
    }

    bool claim(ProcessId pid, std::string& out_name)
    {
        const auto status = slopkit::platform::read_status(pid);
        out_name          = status ? status->name : std::string {};
        return true;
    }

    void shape_modules(std::vector<slopkit::process::ModuleInfo>& modules, ProcessId pid)
    {
        if (const auto exe = slopkit::platform::read_exe(pid))
        {
            (void)slopkit::platform::flag_main_module_by_path(modules, *exe);
        }
    }

    constexpr PluginProfile kProfile {info,
                                      precedence,
                                      access_methods,
                                      claim,
                                      shape_modules,
                                      nullptr,
                                      slopkit::platform::ForeignSignalPolicy::suppress,
                                      nullptr,
                                      nullptr,
                                      nullptr,
                                      nullptr,
                                      nullptr,
                                      nullptr,
                                      nullptr,
                                      nullptr};
}

extern "C" SLOPKIT_PLUGIN_EXPORT const slopkit_plugin_vtable* slopkit_plugin_entry(const slopkit_host_services* host)
{
    return slopkit::plugins::support::entry<kProfile>(host);
}
