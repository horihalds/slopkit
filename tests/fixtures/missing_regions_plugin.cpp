// Test fixture: a plugin built against the pre-`list_regions` ABI. Its vtable is
// one pointer shorter than the host expects, so the loader must reject it with
// the "vtable struct too small" diagnostic instead of reading past the end.

#include "plugin/plugin_api.h"

namespace
{
    slopkit_plugin_vtable make_vtable()
    {
        slopkit_plugin_vtable vtable {};
        vtable.abi_version  = SLOPKIT_PLUGIN_ABI_VERSION;
        vtable.struct_size  = sizeof(slopkit_plugin_vtable) - sizeof(void*);
        vtable.list_regions = nullptr;
        return vtable;
    }

    const slopkit_plugin_vtable g_vtable = make_vtable();
} // namespace

extern "C" SLOPKIT_PLUGIN_EXPORT const slopkit_plugin_vtable*
slopkit_plugin_entry(const slopkit_host_services* /*host*/)
{
    return &g_vtable;
}
