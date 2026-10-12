#pragma once


#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#if defined(_WIN32)
#define SLOPKIT_PLUGIN_EXPORT __declspec(dllexport)
#else
#define SLOPKIT_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#define SLOPKIT_PLUGIN_ABI_VERSION_MAJOR 1
#define SLOPKIT_PLUGIN_ABI_VERSION_MINOR 8
#define SLOPKIT_PLUGIN_ABI_VERSION       ((SLOPKIT_PLUGIN_ABI_VERSION_MAJOR << 16) | SLOPKIT_PLUGIN_ABI_VERSION_MINOR)

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

    enum slopkit_access_method
    {
        SLOPKIT_ACCESS_PROCFS_MEM = 1u << 0,
        SLOPKIT_ACCESS_PROCESS_VM = 1u << 1,
        SLOPKIT_ACCESS_PTRACE     = 1u << 2,
        SLOPKIT_ACCESS_WINESERVER = 1u << 3,
        SLOPKIT_ACCESS_AGENT      = 1u << 4,
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

    enum slopkit_breakpoint_kind
    {
        SLOPKIT_BP_SOFTWARE      = 0,
        SLOPKIT_BP_HW_EXECUTE    = 1,
        SLOPKIT_BP_HW_WRITE      = 2,
        SLOPKIT_BP_HW_READ_WRITE = 3,
    };

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
        uint64_t    entry;
        int32_t     is_main;

        /* The target's pointer width in bytes (4 or 8); 0 when unknown. It sits
           in what used to be trailing padding, so the layout is unchanged. */
        uint32_t pointer_size;
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
        int32_t     shared;
        const char* path;
    } slopkit_region_info;

    typedef struct slopkit_register_value
    {
        const char* name;
        uint64_t    value;
    } slopkit_register_value;

    typedef struct slopkit_stop_info
    {
        int32_t  reason;
        uint32_t tid;
        uint64_t address;
        uint64_t trap_address;
        int32_t  breakpoint_slot;
        int32_t  signal;
    } slopkit_stop_info;

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


        slopkit_result (*debug_attach)(void* session, uint32_t* out_tid);
        slopkit_result (*debug_detach)(void* session);
        slopkit_result (*debug_continue)(void*              session,
                                         uint64_t           resume_address,
                                         size_t             resume_step_size,
                                         slopkit_stop_info* out_stop);
        slopkit_result (*debug_step)(void* session, uint32_t tid, slopkit_stop_info* out_stop);
        slopkit_result (*debug_interrupt)(void* session, uint32_t tid);
        slopkit_result (*debug_get_registers)(void*                    session,
                                              uint32_t                 tid,
                                              slopkit_register_value** out,
                                              size_t*                  out_count);
        slopkit_result (*debug_set_register)(void* session, uint32_t tid, const char* name, uint64_t value);
        slopkit_result (*debug_set_software_breakpoint)(void* session, uint32_t slot, uint64_t address, int32_t insert);
        slopkit_result (*debug_set_hardware_breakpoint)(
            void* session, uint32_t slot, int32_t kind, uint64_t address, size_t size, int32_t insert);
        slopkit_result (*debug_backtrace)(void* session, uint32_t tid, slopkit_frame_info** out, size_t* out_count);


        slopkit_result (*suspend_target)(void* session);
        slopkit_result (*resume_target)(void* session);


        slopkit_result (*allocate_memory)(void* session, uint64_t size, uint64_t near_address, uint64_t* out_address);
        slopkit_result (*free_memory)(void* session, uint64_t address);


        slopkit_result (*validate_memory)(void* session, uint64_t address, uint64_t size, uint32_t* out_valid);
    } slopkit_plugin_vtable;

    SLOPKIT_PLUGIN_EXPORT const slopkit_plugin_vtable* slopkit_plugin_entry(const slopkit_host_services* host);

#ifdef __cplusplus
}
#endif
