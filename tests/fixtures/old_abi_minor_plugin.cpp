// Test fixture: a plugin built against ABI 1.1. Appending `entry` to
// slopkit_module_info in 1.2 changed the module-array element stride, so the
// loader must reject this plugin with the older-minor diagnostic instead of
// reading the array with the wrong stride.

#include "plugin/plugin_api.h"

namespace
{
    slopkit_plugin_vtable make_vtable()
    {
        slopkit_plugin_vtable vtable {};
        vtable.abi_version = (SLOPKIT_PLUGIN_ABI_VERSION_MAJOR << 16) | 1u;
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
