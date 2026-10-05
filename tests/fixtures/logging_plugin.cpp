// Test fixture: a minimal, valid plugin that reports through the host services
// log hook. It logs once during the entry call (before its descriptor id is
// known, so the record must fall back to the library file name) and once when a
// session is opened (after the id is known) so the loader tests can assert
// per-plugin attribution.

#include <cstddef>

#include "plugin/plugin_api.h"

namespace
{
    const slopkit_host_services* g_host = nullptr;

    slopkit_result ok()
    {
        return slopkit_result {SLOPKIT_OK, nullptr};
    }

    slopkit_result fail(int32_t code, const char* message)
    {
        return slopkit_result {code, message};
    }

    const slopkit_plugin_info* info()
    {
        static const slopkit_plugin_info descriptor {
            "logging-fixture", "Logging fixture", "1.0", "reports through the host services log hook"};
        return &descriptor;
    }

    int32_t precedence()
    {
        return 0;
    }

    void* alloc(std::size_t size)
    {
        return g_host != nullptr && g_host->alloc != nullptr ? g_host->alloc(size, g_host->user_data) : nullptr;
    }

    void dealloc(void* memory)
    {
        if (g_host != nullptr && g_host->dealloc != nullptr)
        {
            g_host->dealloc(memory, g_host->user_data);
        }
    }

    slopkit_result list_processes(slopkit_process_info** out, std::size_t* out_count)
    {
        *out       = nullptr;
        *out_count = 0;
        return ok();
    }

    slopkit_result open_session(uint32_t /*pid*/, void** out_session)
    {
        if (g_host != nullptr && g_host->log != nullptr)
        {
            g_host->log(SLOPKIT_LOG_INFO, "opening a session", g_host->user_data);
        }
        *out_session = alloc(1);
        return *out_session != nullptr ? ok() : fail(SLOPKIT_ERR_INTERNAL, "out of memory");
    }

    slopkit_result close_session(void* session)
    {
        dealloc(session);
        return ok();
    }

    slopkit_result read_memory(void* /*session*/,
                               uint64_t /*address*/,
                               void* /*buffer*/,
                               std::size_t /*size*/,
                               std::size_t* out_read,
                               uint32_t*    out_method)
    {
        *out_read   = 0;
        *out_method = 0;
        return fail(SLOPKIT_ERR_IO, "the logging fixture does not read memory");
    }

    slopkit_result write_memory(void* /*session*/,
                                uint64_t /*address*/,
                                const void* /*buffer*/,
                                std::size_t /*size*/,
                                std::size_t* out_written,
                                uint32_t*    out_method)
    {
        *out_written = 0;
        *out_method  = 0;
        return fail(SLOPKIT_ERR_IO, "the logging fixture does not write memory");
    }

    slopkit_result list_modules(void* /*session*/, slopkit_module_info** out, std::size_t* out_count)
    {
        *out       = nullptr;
        *out_count = 0;
        return ok();
    }

    slopkit_result list_threads(void* /*session*/, slopkit_thread_info** out, std::size_t* out_count)
    {
        *out       = nullptr;
        *out_count = 0;
        return ok();
    }

    slopkit_result list_regions(void* /*session*/, slopkit_region_info** out, std::size_t* out_count)
    {
        *out       = nullptr;
        *out_count = 0;
        return ok();
    }

    uint32_t access_methods()
    {
        return SLOPKIT_ACCESS_PROCFS_MEM;
    }
} // namespace

extern "C" SLOPKIT_PLUGIN_EXPORT const slopkit_plugin_vtable* slopkit_plugin_entry(const slopkit_host_services* host)
{
    g_host = host;

    if (host != nullptr && host->log != nullptr)
    {
        host->log(SLOPKIT_LOG_WARN, "loaded without a descriptor id yet", host->user_data);
    }

    static const slopkit_plugin_vtable vtable {
        SLOPKIT_PLUGIN_ABI_VERSION,
        sizeof(slopkit_plugin_vtable),
        &info,
        &precedence,
        &list_processes,
        &open_session,
        &close_session,
        &read_memory,
        &write_memory,
        &list_modules,
        &list_threads,
        &list_regions,
        &access_methods,
        // The fixture does not debug; the host must never call these.
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
    };
    return &vtable;
}
