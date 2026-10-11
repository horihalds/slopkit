#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "platform/linux/debug_session.hpp"
#include "plugin/plugin_api.h"
#include "plugins/support/allocate.hpp"
#include "process/types.hpp"

namespace slopkit::plugins::support
{
    // What one bundled plugin supplies; everything else comes from this library.
    struct PluginProfile
    {
        // Identity: id, name, version and description.
        const slopkit_plugin_info* (*info)() noexcept;
        // Ordering among the plugins that claim the same process.
        std::int32_t (*precedence)() noexcept;
        // The access primitives the plugin advertises.
        std::uint32_t (*access_methods)() noexcept;
        // Claims a pid and returns the name the picker shows; false when the
        // plugin does not claim it (Wine filters here and falls back to the exe
        // filename for the name).
        bool (*claim)(process::ProcessId pid, std::string& out_name);
        // Reshapes the module list the shared listing built: Wine keeps the PE
        // images and flags the main PE, linux-proc flags the exe-backed module.
        void (*shape_modules)(std::vector<process::ModuleInfo>& modules, process::ProcessId pid);
        // What the debug session does with the target's own signals.
        platform::ForeignSignalPolicy signal_policy;
        // Optional: the plugin's own allocator, tried before this library's
        // ptrace path. Wine/Proton uses it to allocate inside the target through
        // its resident agent, so Wine's VM manager knows about the mapping.
        // nullptr means the shared ptrace allocator is used directly.
        std::expected<std::uint64_t, AllocationError> (*allocate)(Session&      session,
                                                                  std::uint64_t size,
                                                                  std::uint64_t near_address);
        // Optional: pairs with `allocate` and releases an address it handed out.
        std::expected<void, AllocationError> (*free)(Session& session, std::uint64_t address);
        // Optional: run before the session is destroyed, so a plugin's in-target
        // state (Wine's resident agent) is shut down and unmapped.
        void (*close_session)(Session& session);
    };

    // The single exported symbol a plugin needs: installs the host services and
    // this plugin's profile, then returns the plugin's static vtable. The
    // plumbing that fills the vtable lives in this library, which is linked once
    // into each plugin. The loader dlopen's plugins with RTLD_LOCAL, so each
    // plugin library instance keeps its own copy of the state (host services,
    // string arena and session registry) and two plugins never share it.
    [[nodiscard]] const slopkit_plugin_vtable* install(const slopkit_host_services* host,
                                                       const PluginProfile*         profile) noexcept;

    // The one-liner a plugin's entry point expands to.
    template<const PluginProfile& Profile>
    [[nodiscard]] const slopkit_plugin_vtable* entry(const slopkit_host_services* host) noexcept
    {
        return install(host, &Profile);
    }

} // namespace slopkit::plugins::support
