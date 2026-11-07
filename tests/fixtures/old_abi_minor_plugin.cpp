
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
}

extern "C" SLOPKIT_PLUGIN_EXPORT const slopkit_plugin_vtable* slopkit_plugin_entry(const slopkit_host_services*)
{
    return &g_vtable;
}
