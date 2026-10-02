// wine-proton: process access for games and software running under Wine or
// Steam Proton.
//
// Claims only processes detected as Wine/Proton, exposes their PE images as
// modules, and reads/writes memory through the same ptrace-free primitives as
// linux-proc.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <new>
#include <span>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "platform/linux/memory.hpp"
#include "platform/linux/procfs.hpp"
#include "platform/linux/wine.hpp"
#include "plugin/plugin_api.h"

namespace platform = slopkit::platform;

namespace
{
    const slopkit_host_services* g_host = nullptr;

    struct Session
    {
        slopkit::process::ProcessId pid {};
    };

    std::unordered_set<Session*>& sessions()
    {
        static std::unordered_set<Session*> value;
        return value;
    }

    std::deque<std::string>& string_arena()
    {
        static std::deque<std::string> value;
        return value;
    }

    void reset_arena()
    {
        string_arena().clear();
    }

    const char* intern(std::string value)
    {
        string_arena().push_back(std::move(value));
        return string_arena().back().c_str();
    }

    void* host_alloc(std::size_t size)
    {
        if (g_host == nullptr || g_host->alloc == nullptr)
        {
            return nullptr;
        }
        return g_host->alloc(size, g_host->user_data);
    }

    void host_dealloc(void* memory)
    {
        if (memory == nullptr || g_host == nullptr || g_host->dealloc == nullptr)
        {
            return;
        }
        g_host->dealloc(memory, g_host->user_data);
    }

    slopkit_result ok()
    {
        return slopkit_result {SLOPKIT_OK, nullptr};
    }

    slopkit_result fail(int32_t code, const char* message)
    {
        return slopkit_result {code, message};
    }

    Session* lookup(void* handle)
    {
        auto* session = static_cast<Session*>(handle);
        if (session == nullptr || !sessions().contains(session))
        {
            return nullptr;
        }
        return session;
    }

    int32_t status_for(platform::MemoryError error)
    {
        switch (error)
        {
        case platform::MemoryError::process_not_found:
            return SLOPKIT_ERR_NOT_FOUND;
        case platform::MemoryError::permission_denied:
            return SLOPKIT_ERR_PERMISSION_DENIED;
        case platform::MemoryError::invalid_argument:
            return SLOPKIT_ERR_INVALID_ARGUMENT;
        case platform::MemoryError::unmapped:
            return SLOPKIT_ERR_NOT_FOUND;
        case platform::MemoryError::io_error:
            return SLOPKIT_ERR_IO;
        }
        return SLOPKIT_ERR_INTERNAL;
    }

    const char* message_for(platform::MemoryError error)
    {
        switch (error)
        {
        case platform::MemoryError::process_not_found:
            return "no such process";
        case platform::MemoryError::permission_denied:
            return "permission denied";
        case platform::MemoryError::invalid_argument:
            return "invalid argument";
        case platform::MemoryError::unmapped:
            return "address range is not mapped";
        case platform::MemoryError::io_error:
            return "memory I/O error";
        }
        return "unknown memory error";
    }

    uint32_t method_flag(platform::MemoryPrimitive primitive)
    {
        switch (primitive)
        {
        case platform::MemoryPrimitive::process_vm:
            return SLOPKIT_ACCESS_PROCESS_VM;
        case platform::MemoryPrimitive::procfs_mem:
            return SLOPKIT_ACCESS_PROCFS_MEM;
        case platform::MemoryPrimitive::none:
            return 0;
        }
        return 0;
    }

    std::string process_name(slopkit::process::ProcessId pid)
    {
        if (const auto status = platform::read_status(pid))
        {
            if (!status->name.empty())
            {
                return status->name;
            }
        }
        if (const auto exe = platform::read_exe(pid))
        {
            return std::filesystem::path(*exe).filename().string();
        }
        return {};
    }

    const slopkit_plugin_info* plugin_info() noexcept
    {
        static const slopkit_plugin_info info {
            "wine-proton",
            "Wine/Proton process access",
            "0.1.0",
            "Claims Wine and Steam Proton processes and exposes their PE images without ptrace."};
        return &info;
    }

    int32_t plugin_precedence() noexcept
    {
        // Lower than linux-proc, so it is the default for a Wine process both
        // plugins can see.
        return 10;
    }

    uint32_t plugin_access_methods() noexcept
    {
        return SLOPKIT_ACCESS_PROCESS_VM | SLOPKIT_ACCESS_PROCFS_MEM;
    }

    slopkit_result plugin_list_processes(slopkit_process_info** out, std::size_t* out_count) noexcept
    {
        try
        {
            if (out == nullptr || out_count == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "null output pointer");
            }
            *out       = nullptr;
            *out_count = 0;
            reset_arena();

            std::vector<slopkit::process::ProcessId> claimed;
            for (const auto pid : platform::list_pids())
            {
                if (platform::classify_wine_process(pid).claimed())
                {
                    claimed.push_back(pid);
                }
            }

            auto* array = static_cast<slopkit_process_info*>(host_alloc(sizeof(slopkit_process_info) * claimed.size()));
            if (array == nullptr && !claimed.empty())
            {
                return fail(SLOPKIT_ERR_INTERNAL, "allocation failed");
            }

            std::size_t count = 0;
            for (const auto pid : claimed)
            {
                const auto exe        = platform::read_exe(pid);
                array[count].pid      = pid;
                array[count].name     = intern(process_name(pid));
                array[count].exe_path = exe ? intern(*exe) : nullptr;
                ++count;
            }

            *out       = array;
            *out_count = count;
            return ok();
        }
        catch (...)
        {
            return fail(SLOPKIT_ERR_INTERNAL, "unhandled exception");
        }
    }

    slopkit_result plugin_open_session(uint32_t pid, void** out_session) noexcept
    {
        try
        {
            if (out_session == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "null output pointer");
            }
            *out_session = nullptr;

            std::error_code error;
            if (!std::filesystem::exists("/proc/" + std::to_string(pid), error))
            {
                return fail(SLOPKIT_ERR_NOT_FOUND, "no such process");
            }

            auto* session = static_cast<Session*>(host_alloc(sizeof(Session)));
            if (session == nullptr)
            {
                return fail(SLOPKIT_ERR_INTERNAL, "allocation failed");
            }
            new (session) Session {pid};
            sessions().insert(session);
            *out_session = session;
            return ok();
        }
        catch (...)
        {
            return fail(SLOPKIT_ERR_INTERNAL, "unhandled exception");
        }
    }

    slopkit_result plugin_close_session(void* handle) noexcept
    {
        try
        {
            auto* session = lookup(handle);
            if (session == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
            }
            sessions().erase(session);
            session->~Session();
            host_dealloc(session);
            return ok();
        }
        catch (...)
        {
            return fail(SLOPKIT_ERR_INTERNAL, "unhandled exception");
        }
    }

    slopkit_result plugin_read_memory(void*        handle,
                                      uint64_t     address,
                                      void*        buffer,
                                      std::size_t  size,
                                      std::size_t* out_read,
                                      uint32_t*    out_method) noexcept
    {
        try
        {
            if (out_read != nullptr)
            {
                *out_read = 0;
            }
            if (out_method != nullptr)
            {
                *out_method = 0;
            }

            auto* session = lookup(handle);
            if (session == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
            }
            if (size == 0)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "zero-length read");
            }
            if (buffer == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "null buffer");
            }

            const auto destination = std::span<std::byte>(static_cast<std::byte*>(buffer), size);
            const auto result      = platform::read_memory(session->pid, address, destination);
            if (!result)
            {
                return fail(status_for(result.error()), message_for(result.error()));
            }
            if (out_read != nullptr)
            {
                *out_read = result->bytes;
            }
            if (out_method != nullptr)
            {
                *out_method = method_flag(result->primitive);
            }
            return ok();
        }
        catch (...)
        {
            return fail(SLOPKIT_ERR_INTERNAL, "unhandled exception");
        }
    }

    slopkit_result plugin_write_memory(void*        handle,
                                       uint64_t     address,
                                       const void*  buffer,
                                       std::size_t  size,
                                       std::size_t* out_written,
                                       uint32_t*    out_method) noexcept
    {
        try
        {
            if (out_written != nullptr)
            {
                *out_written = 0;
            }
            if (out_method != nullptr)
            {
                *out_method = 0;
            }

            auto* session = lookup(handle);
            if (session == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
            }
            if (size == 0)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "zero-length write");
            }
            if (buffer == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "null buffer");
            }

            const auto source = std::span<const std::byte>(static_cast<const std::byte*>(buffer), size);
            const auto result = platform::write_memory(session->pid, address, source);
            if (!result)
            {
                return fail(status_for(result.error()), message_for(result.error()));
            }
            if (out_written != nullptr)
            {
                *out_written = result->bytes;
            }
            if (out_method != nullptr)
            {
                *out_method = method_flag(result->primitive);
            }
            return ok();
        }
        catch (...)
        {
            return fail(SLOPKIT_ERR_INTERNAL, "unhandled exception");
        }
    }

    slopkit_result plugin_list_modules(void* handle, slopkit_module_info** out, std::size_t* out_count) noexcept
    {
        try
        {
            if (out == nullptr || out_count == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "null output pointer");
            }
            *out       = nullptr;
            *out_count = 0;

            auto* session = lookup(handle);
            if (session == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
            }
            reset_arena();

            // Wine exposes the target's PE images through the process maps; keep
            // only the PE ones so the module list is game-centric.
            std::vector<slopkit::process::ModuleInfo> images;
            for (auto& module : platform::modules_from_maps(platform::read_maps(session->pid)))
            {
                if (module.kind == slopkit::process::ModuleKind::pe)
                {
                    images.push_back(std::move(module));
                }
            }

            auto* array = static_cast<slopkit_module_info*>(host_alloc(sizeof(slopkit_module_info) * images.size()));
            if (array == nullptr && !images.empty())
            {
                return fail(SLOPKIT_ERR_INTERNAL, "allocation failed");
            }

            std::size_t count = 0;
            for (const auto& image : images)
            {
                array[count].base   = image.base;
                array[count].size   = image.size;
                array[count].offset = image.offset;
                array[count].kind   = static_cast<int32_t>(image.kind);
                array[count].name   = intern(image.name);
                array[count].path   = image.path.empty() ? nullptr : intern(image.path);
                ++count;
            }

            *out       = array;
            *out_count = count;
            return ok();
        }
        catch (...)
        {
            return fail(SLOPKIT_ERR_INTERNAL, "unhandled exception");
        }
    }

    slopkit_result plugin_list_threads(void* handle, slopkit_thread_info** out, std::size_t* out_count) noexcept
    {
        try
        {
            if (out == nullptr || out_count == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "null output pointer");
            }
            *out       = nullptr;
            *out_count = 0;

            auto* session = lookup(handle);
            if (session == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
            }
            reset_arena();

            const auto threads = platform::read_threads(session->pid);
            auto* array = static_cast<slopkit_thread_info*>(host_alloc(sizeof(slopkit_thread_info) * threads.size()));
            if (array == nullptr && !threads.empty())
            {
                return fail(SLOPKIT_ERR_INTERNAL, "allocation failed");
            }

            std::size_t count = 0;
            for (const auto& thread : threads)
            {
                array[count].tid  = thread.tid;
                array[count].name = thread.name.empty() ? nullptr : intern(thread.name);
                ++count;
            }

            *out       = array;
            *out_count = count;
            return ok();
        }
        catch (...)
        {
            return fail(SLOPKIT_ERR_INTERNAL, "unhandled exception");
        }
    }

    const slopkit_plugin_vtable g_vtable {
        SLOPKIT_PLUGIN_ABI_VERSION,
        sizeof(slopkit_plugin_vtable),
        plugin_info,
        plugin_precedence,
        plugin_list_processes,
        plugin_open_session,
        plugin_close_session,
        plugin_read_memory,
        plugin_write_memory,
        plugin_list_modules,
        plugin_list_threads,
        plugin_access_methods,
    };
} // namespace

extern "C" SLOPKIT_PLUGIN_EXPORT const slopkit_plugin_vtable* slopkit_plugin_entry(const slopkit_host_services* host)
{
    g_host = host;
    return &g_vtable;
}
