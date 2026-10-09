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
#define SLOPKIT_PLUGIN_ABI_VERSION_MINOR 6
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

    /* Breakpoint kinds for the debug operations below. A software breakpoint
       is a one-byte int3 written into the target; the hardware kinds use the
       ordinary x86-64 debug slots DR0-DR3. */
    enum slopkit_breakpoint_kind
    {
        SLOPKIT_BP_SOFTWARE      = 0,
        SLOPKIT_BP_HW_EXECUTE    = 1,
        SLOPKIT_BP_HW_WRITE      = 2,
        SLOPKIT_BP_HW_READ_WRITE = 3,
    };

    /* Why a traced thread stopped, carried in slopkit_stop_info::reason. */
    enum slopkit_stop_reason
    {
        SLOPKIT_STOP_BREAKPOINT  = 0,
        SLOPKIT_STOP_SINGLE_STEP = 1,
        SLOPKIT_STOP_INTERRUPT   = 2,
        SLOPKIT_STOP_EXITED      = 3,
        SLOPKIT_STOP_SIGNALED    = 4,
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
        /* Non-zero when the plugin reports this image as the process' main image
           (the image it was started from). At most one entry per list. */
        int32_t     is_main;
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

    /* One register of a stopped thread; `name` points at a plugin-owned
       literal and stays valid for the process' lifetime. */
    typedef struct slopkit_register_value
    {
        const char* name;
        uint64_t    value;
    } slopkit_register_value;

    /* One stop event of a traced thread. `trap_address` is the int3 address of
       a software trap or the watched address of a hardware breakpoint;
       `breakpoint_slot` is the fired DR slot (0-3) or -1 when none fired. */
    typedef struct slopkit_stop_info
    {
        int32_t  reason;
        uint32_t tid;
        uint64_t address;
        uint64_t trap_address;
        int32_t  breakpoint_slot;
        int32_t  signal;
    } slopkit_stop_info;

    /* One raw stack frame of a stopped thread. All labelling stays in the host. */
    typedef struct slopkit_frame_info
    {
        uint64_t pc;
        uint64_t frame_pointer;
    } slopkit_frame_info;

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
     * expects. The host requires the current minor because appending `entry` and
     * `is_main` to slopkit_module_info changed the element stride of the module
     * array, so a plugin built against an older minor would be misread.
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

        /* --- debug operations (ABI 1.4), appended so a plugin built against
           an older minor keeps loading with these left null. A plugin that
           leaves them null simply cannot debug; the host reports
           "unsupported" and never calls them. --- */

        /* Seizes and stops every thread of the session's process and returns
           the thread-group leader. Idempotent while a session is open. */
        slopkit_result (*debug_attach)(void* session, uint32_t* out_tid);
        /* Detaches every thread, restores the original instruction bytes and
           clears the debug slots. Idempotent. */
        slopkit_result (*debug_detach)(void* session);
        /* Resumes the whole thread group and blocks until one thread stops.
           With a non-zero `resume_address` the saved byte at that address is
           restored, the current thread is single-stepped and the trap is
           re-inserted, so a software breakpoint can be stepped over. */
        slopkit_result (*debug_continue)(void*              session,
                                         uint64_t           resume_address,
                                         size_t             resume_step_size,
                                         slopkit_stop_info* out_stop);
        /* Single-steps one thread and blocks until it stops. */
        slopkit_result (*debug_step)(void* session, uint32_t tid, slopkit_stop_info* out_stop);
        /* Asks a running thread to stop. It does not block: the in-flight
           debug_continue observes the interrupt and reports it. */
        slopkit_result (*debug_interrupt)(void* session, uint32_t tid);
        /* The 18 x86-64 registers of a stopped thread, allocated with the host
           allocator and released by the host. */
        slopkit_result (*debug_get_registers)(void*                    session,
                                              uint32_t                 tid,
                                              slopkit_register_value** out,
                                              size_t*                  out_count);
        slopkit_result (*debug_set_register)(void* session, uint32_t tid, const char* name, uint64_t value);
        /* Arms (`insert` non-zero) or disarms a software trap at a slot the
           host allocates; the original byte is saved by the plugin. */
        slopkit_result (*debug_set_software_breakpoint)(void* session, uint32_t slot, uint64_t address, int32_t insert);
        /* Arms or disarms a hardware breakpoint in DR slot 0-3. `size` is 1, 2,
           4 or 8; an execute breakpoint is always length 1. */
        slopkit_result (*debug_set_hardware_breakpoint)(
            void* session, uint32_t slot, int32_t kind, uint64_t address, size_t size, int32_t insert);
        /* The raw stack frames of a stopped thread, top first. */
        slopkit_result (*debug_backtrace)(void* session, uint32_t tid, slopkit_frame_info** out, size_t* out_count);

        /* --- suspend operations (ABI 1.5), appended so a plugin built against
           an older minor keeps loading with these left null. A plugin that
           leaves them null simply cannot suspend; the host reports
           "unsupported" and never calls them. --- */

        /* Stops every thread of the session's target with SIGSTOP. Its memory
           stays readable. Idempotent. */
        slopkit_result (*suspend_target)(void* session);
        /* Resumes a target stopped by suspend_target with SIGCONT. Idempotent. */
        slopkit_result (*resume_target)(void* session);

        /* --- target memory allocation (ABI 1.6), appended so a plugin built
           against an older minor keeps loading with these left null. A plugin
           that leaves them null cannot allocate; the host reports
           "unsupported" and never calls them. --- */

        /* Maps `size` bytes (rounded up to the target's page size) in the
           target with at least read/write/execute permission and stores the
           mapping base in *out_address. `near_address` is a best-effort hint
           (0 = anywhere): the plugin places the mapping as close to it as it
           can (the hint itself, else free space below it, else above it, else
           anywhere). A hint the plugin cannot honour is not an error while the
           mapping succeeds. */
        slopkit_result (*allocate_memory)(void* session, uint64_t size, uint64_t near_address, uint64_t* out_address);
        /* Unmaps the whole mapping an earlier allocate_memory of the same
           session returned at `address`; anything else is SLOPKIT_ERR_NOT_FOUND. */
        slopkit_result (*free_memory)(void* session, uint64_t address);
    } slopkit_plugin_vtable;

    /*
     * The single symbol a plugin must export. Returns a pointer to a vtable that
     * lives as long as the plugin, or NULL on failure.
     */
    SLOPKIT_PLUGIN_EXPORT const slopkit_plugin_vtable* slopkit_plugin_entry(const slopkit_host_services* host);

#ifdef __cplusplus
}
#endif
