// Test fixture: a plugin whose ABI major version does not match the host's. The
// loader must reject it with a diagnostic instead of aborting.

#include "plugin/plugin_api.h"

namespace
{
    slopkit_plugin_vtable make_vtable()
    {
        slopkit_plugin_vtable vtable {};
        vtable.abi_version = 2u << 16; // Deliberately incompatible major version.
        vtable.struct_size = sizeof(slopkit_plugin_vtable);
        return vtable;
    }

    const slopkit_plugin_vtable g_vtable = make_vtable();
} // namespace

extern "C" SLOPKIT_PLUGIN_EXPORT const slopkit_plugin_vtable*
slopkit_plugin_entry(const slopkit_host_services* /*host*/)
{
    return &g_vtable;
}
