#pragma once

/*
 * slopkit process-access plugin ABI.
 *
 * This header is the contract between the slopkit host and a plugin. It is
 * plain C so that plugins can be written in C (or any language able to export a
 * C symbol) and loaded with dlopen. A plugin implements exactly one entry
 * point, `slopkit_plugin_entry`, and returns a pointer to a static vtable.
 *
 * Conventions:
 *  - Functions return `slopkit_result`; a non-zero `code` is an error and
 *    `message` may carry a human-readable detail.
 *  - Any string a plugin returns is owned by the plugin and must stay valid at
 *    least until the next call on the same plugin (or until unload for
 *    descriptor strings).
 *  - Buffers returned by the list* functions are allocated with the host
 *    services allocator and must therefore be released by the host with the
 *    host services deallocator.
 *  - No C++ exception may cross this boundary. C++ plugins must guard every
 *    exported function with try/catch.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Marks the single symbol a plugin exports. Plugins are built with hidden
   visibility, so the entry point must opt back in explicitly. */
#if defined(_WIN32)
#define SLOPKIT_PLUGIN_EXPORT __declspec(dllexport)
#else
#define SLOPKIT_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#define SLOPKIT_PLUGIN_ABI_VERSION_MAJOR 1
#define SLOPKIT_PLUGIN_ABI_VERSION_MINOR 2
#define SLOPKIT_PLUGIN_ABI_VERSION       ((SLOPKIT_PLUGIN_ABI_VERSION_MAJOR << 16) | SLOPKIT_PLUGIN_ABI_VERSION_MINOR)

    /* Status codes carried in `slopkit_result::code`. */
    enum slopkit_status
    {
        SLOPKIT_OK                    = 0,
        SLOPKIT_ERR_PERMISSION_DENIED = 1,
        SLOPKIT_ERR_NOT_FOUND         = 2,
        SLOPKIT_ERR_UNSUPPORTED       = 3,
        SLOPKIT_ERR_IO                = 4,
        SLOPKIT_ERR_INVALID_ABI       = 5,
        SLOPKIT_ERR_INVALID_ARGUMENT  = 6,
        SLOPKIT_ERR_INTERNAL          = 7,
    };

    /* Access primitives a plugin can use, advertised as a bit mask. */
    enum slopkit_access_method
    {
        SLOPKIT_ACCESS_PROCFS_MEM = 1u << 0,
        SLOPKIT_ACCESS_PROCESS_VM = 1u << 1,
        SLOPKIT_ACCESS_PTRACE     = 1u << 2,
        SLOPKIT_ACCESS_WINESERVER = 1u << 3,
    };

    enum slopkit_module_kind
    {
        SLOPKIT_MODULE_ELF       = 0,
        SLOPKIT_MODULE_PE        = 1,
        SLOPKIT_MODULE_ANONYMOUS = 2,
    };

    enum slopkit_log_level
    {
        SLOPKIT_LOG_DEBUG = 0,
        SLOPKIT_LOG_INFO  = 1,
        SLOPKIT_LOG_WARN  = 2,
        SLOPKIT_LOG_ERROR = 3,
    };

    typedef struct slopkit_result
    {
        int32_t     code;
        /* Plugin-owned detail, valid at least until the next plugin call. May be NULL. */
        const char* message;
    } slopkit_result;

    typedef struct slopkit_host_services
    {
        uint32_t abi_version;
        uint32_t struct_size;
        void*    user_data;
        void* (*alloc)(size_t size, void* user_data);
        void (*dealloc)(void* memory, void* user_data);
        void (*log)(int32_t level, const char* message, void* user_data);
    } slopkit_host_services;

    typedef struct slopkit_process_info
    {
        uint32_t    pid;
        const char* name;
        const char* exe_path;
    } slopkit_process_info;

    typedef struct slopkit_module_info
    {
        uint64_t    base;
        uint64_t    size;
        uint64_t    offset;
        int32_t     kind;
        const char* name;
        const char* path;
        /* Absolute entry point of the image; 0 when the plugin does not know it. */
        uint64_t    entry;
    } slopkit_module_info;

    typedef struct slopkit_thread_info
    {
        uint32_t    tid;
        const char* name;
    } slopkit_thread_info;

    typedef struct slopkit_region_info
    {
        uint64_t    start;
        uint64_t    end;
        uint64_t    offset;
        int32_t     readable;
        int32_t     writable;
        int32_t     executable;
        /* Non-zero when the mapping is shared; false plus a path is copy-on-write. */
        int32_t     shared;
        const char* path;
    } slopkit_region_info;

    typedef struct slopkit_plugin_info
    {
        const char* id;
        const char* name;
        const char* version;
        const char* description;
    } slopkit_plugin_info;

    /*
     * The plugin vtable. `abi_version` and `struct_size` must be the first two
     * fields; the host checks them before reading anything else, and rejects a
     * major-version mismatch, an older minor version or a struct smaller than it
     * expects. The host requires the current minor because appending `entry` to
     * slopkit_module_info changed the element stride of the module array, so a
     * plugin built against an older minor would be misread.
     */
    typedef struct slopkit_plugin_vtable
    {
        uint32_t abi_version;
        uint32_t struct_size;

        const slopkit_plugin_info* (*info)(void);
        int32_t (*precedence)(void);
        slopkit_result (*list_processes)(slopkit_process_info** out, size_t* out_count);
        slopkit_result (*open_session)(uint32_t pid, void** out_session);
        slopkit_result (*close_session)(void* session);
        slopkit_result (*read_memory)(
            void* session, uint64_t address, void* buffer, size_t size, size_t* out_read, uint32_t* out_method);
        slopkit_result (*write_memory)(void*       session,
                                       uint64_t    address,
                                       const void* buffer,
                                       size_t      size,
                                       size_t*     out_written,
                                       uint32_t*   out_method);
        slopkit_result (*list_modules)(void* session, slopkit_module_info** out, size_t* out_count);
        slopkit_result (*list_threads)(void* session, slopkit_thread_info** out, size_t* out_count);
        slopkit_result (*list_regions)(void* session, slopkit_region_info** out, size_t* out_count);
        uint32_t (*access_methods)(void);
    } slopkit_plugin_vtable;

    /*
     * The single symbol a plugin must export. Returns a pointer to a vtable that
     * lives as long as the plugin, or NULL on failure.
     */
    SLOPKIT_PLUGIN_EXPORT const slopkit_plugin_vtable* slopkit_plugin_entry(const slopkit_host_services* host);

#ifdef __cplusplus
}
#endif
