// linux-proc: process access for any normal Linux process.
//
// Enumerates /proc, resolves module regions and threads, and reads/writes memory
// through process_vm_readv/process_vm_writev with a /proc/<pid>/mem fallback.
// The read/write path never calls ptrace, so nothing like TracerPid appears in
// the target. The opt-in debugger operations (ABI 1.4) do use ptrace, but only
// while an explicitly started debug session is open.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <format>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "platform/linux/memory.hpp"
#include "platform/linux/procfs.hpp"
#include "platform/linux/ptrace.hpp"
#include "plugin/plugin_api.h"

namespace platform = slopkit::platform;

namespace
{
    const slopkit_host_services* g_host = nullptr;

    // Software breakpoint slots the host allocates; the plugin only stores the
    // replaced byte and whether the trap is currently written.
    struct SoftwareSlot
    {
        bool          armed {};
        std::uint64_t address {};
        std::uint64_t original {};
        std::size_t   size {};
    };

    struct HardwareSlot
    {
        bool          armed {};
        std::int32_t  kind {SLOPKIT_BP_HW_EXECUTE};
        std::uint64_t address {};
        std::size_t   size {1};
    };

    constexpr std::size_t kSoftwareSlotCount = 64;
    constexpr std::size_t kHardwareSlotCount = 4;

    // The opt-in debug session: which threads are seized and which breakpoint
    // slots are armed. Only alive while an explicit debug session is open.
    struct DebugState
    {
        bool                                         attached {};
        slopkit::process::ProcessId                  leader {};
        slopkit::process::ProcessId                  current {};
        std::vector<slopkit::process::ProcessId>     tids;
        std::array<SoftwareSlot, kSoftwareSlotCount> software {};
        std::array<HardwareSlot, kHardwareSlotCount> hardware {};
    };

    struct Session
    {
        explicit Session(slopkit::process::ProcessId process_id) : pid(process_id), mem(process_id) {}

        slopkit::process::ProcessId pid {};
        platform::MemAccess         mem;

        // Debug state is touched from the debug worker's job thread and its run
        // thread, so every access is guarded; the blocking wait is done without
        // the lock held.
        std::mutex debug_mutex;
        DebugState debug;
    };

    std::mutex& sessions_mutex()
    {
        static std::mutex value;
        return value;
    }

    std::unordered_set<Session*>& sessions()
    {
        static std::unordered_set<Session*> value;
        return value;
    }

    // Strings handed to the host stay valid until the next call; the arena is
    // per host thread because two host threads can call into this plugin at
    // once. std::deque keeps references stable across push_back.
    std::deque<std::string>& string_arena()
    {
        thread_local std::deque<std::string> value;
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

    // Reports through the host log hook; the host tags the record with this
    // plugin's id. A message must outlive the call, which a local string does.
    void log_message(int32_t level, const std::string& text)
    {
        if (g_host != nullptr && g_host->log != nullptr)
        {
            g_host->log(level, text.c_str(), g_host->user_data);
        }
    }

    Session* lookup(void* handle)
    {
        auto* session = static_cast<Session*>(handle);
        if (session == nullptr)
        {
            return nullptr;
        }
        const std::lock_guard lock(sessions_mutex());
        return sessions().contains(session) ? session : nullptr;
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

    const slopkit_plugin_info* plugin_info() noexcept
    {
        static const slopkit_plugin_info info {
            "linux-proc",
            "Linux process access",
            "0.2.0",
            "Enumerates any accessible Linux process, reads/writes its memory without ptrace and can run an "
            "opt-in ptrace debug session."};
        return &info;
    }

    int32_t plugin_precedence() noexcept
    {
        return 100;
    }

    uint32_t plugin_access_methods() noexcept
    {
        // ptrace is advertised because the plugin can run the opt-in debugger;
        // the read/write path still reports process_vm/procfs per session.
        return SLOPKIT_ACCESS_PROCESS_VM | SLOPKIT_ACCESS_PROCFS_MEM | SLOPKIT_ACCESS_PTRACE;
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

            const auto pids = platform::list_pids();
            auto* array = static_cast<slopkit_process_info*>(host_alloc(sizeof(slopkit_process_info) * pids.size()));
            if (array == nullptr && !pids.empty())
            {
                return fail(SLOPKIT_ERR_INTERNAL, "allocation failed");
            }

            std::size_t count = 0;
            for (const auto pid : pids)
            {
                const auto status     = platform::read_status(pid);
                const auto exe        = platform::read_exe(pid);
                array[count].pid      = pid;
                array[count].name     = intern(status ? status->name : std::string {});
                array[count].exe_path = exe ? intern(*exe) : nullptr;
                ++count;
            }

            log_message(SLOPKIT_LOG_DEBUG, std::format("listed {} process(es)", count));

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
                log_message(SLOPKIT_LOG_WARN, std::format("cannot attach to pid {}: no such process", pid));
                return fail(SLOPKIT_ERR_NOT_FOUND, "no such process");
            }

            auto* session = static_cast<Session*>(host_alloc(sizeof(Session)));
            if (session == nullptr)
            {
                return fail(SLOPKIT_ERR_INTERNAL, "allocation failed");
            }
            new (session) Session {pid};
            {
                const std::lock_guard lock(sessions_mutex());
                sessions().insert(session);
            }
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
            {
                const std::lock_guard lock(sessions_mutex());
                sessions().erase(session);
            }
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
            const auto result      = session->mem.read(address, destination);
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
            const auto result = session->mem.write(address, source);
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

            auto modules = platform::modules_from_maps(platform::read_maps(session->pid));
            platform::fill_module_entry_points(session->pid, modules);
            if (const auto exe = platform::read_exe(session->pid))
            {
                (void)platform::flag_main_module_by_path(modules, *exe);
            }
            auto* array = static_cast<slopkit_module_info*>(host_alloc(sizeof(slopkit_module_info) * modules.size()));
            if (array == nullptr && !modules.empty())
            {
                return fail(SLOPKIT_ERR_INTERNAL, "allocation failed");
            }

            std::size_t count = 0;
            for (const auto& module : modules)
            {
                array[count].base    = module.base;
                array[count].size    = module.size;
                array[count].offset  = module.offset;
                array[count].kind    = static_cast<int32_t>(module.kind);
                array[count].name    = intern(module.name);
                array[count].path    = module.path.empty() ? nullptr : intern(module.path);
                array[count].entry   = module.entry;
                array[count].is_main = module.is_main ? 1 : 0;
                ++count;
            }

            log_message(SLOPKIT_LOG_DEBUG, std::format("listed {} module(s) for pid {}", count, session->pid));

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

            log_message(SLOPKIT_LOG_DEBUG, std::format("listed {} thread(s) for pid {}", count, session->pid));

            *out       = array;
            *out_count = count;
            return ok();
        }
        catch (...)
        {
            return fail(SLOPKIT_ERR_INTERNAL, "unhandled exception");
        }
    }

    slopkit_result plugin_list_regions(void* handle, slopkit_region_info** out, std::size_t* out_count) noexcept
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

            const auto regions = platform::read_maps(session->pid);
            auto* array = static_cast<slopkit_region_info*>(host_alloc(sizeof(slopkit_region_info) * regions.size()));
            if (array == nullptr && !regions.empty())
            {
                return fail(SLOPKIT_ERR_INTERNAL, "allocation failed");
            }

            std::size_t count = 0;
            for (const auto& region : regions)
            {
                array[count].start      = region.start;
                array[count].end        = region.end;
                array[count].offset     = region.offset;
                array[count].readable   = region.readable ? 1 : 0;
                array[count].writable   = region.writable ? 1 : 0;
                array[count].executable = region.executable ? 1 : 0;
                array[count].shared     = region.shared ? 1 : 0;
                array[count].path       = region.path.empty() ? nullptr : intern(region.path);
                ++count;
            }

            log_message(SLOPKIT_LOG_DEBUG, std::format("listed {} region(s) for pid {}", count, session->pid));

            *out       = array;
            *out_count = count;
            return ok();
        }
        catch (...)
        {
            return fail(SLOPKIT_ERR_INTERNAL, "unhandled exception");
        }
    }

    // --- debug operations (ABI 1.4) ----------------------------------------

    int32_t status_for(slopkit::process::AccessError error)
    {
        switch (error)
        {
        case slopkit::process::AccessError::permission_denied:
            return SLOPKIT_ERR_PERMISSION_DENIED;
        case slopkit::process::AccessError::not_found:
            return SLOPKIT_ERR_NOT_FOUND;
        case slopkit::process::AccessError::unsupported:
            return SLOPKIT_ERR_UNSUPPORTED;
        case slopkit::process::AccessError::io_error:
            return SLOPKIT_ERR_IO;
        case slopkit::process::AccessError::invalid_abi:
            return SLOPKIT_ERR_INVALID_ABI;
        case slopkit::process::AccessError::invalid_argument:
            return SLOPKIT_ERR_INVALID_ARGUMENT;
        case slopkit::process::AccessError::internal:
            break;
        }
        return SLOPKIT_ERR_INTERNAL;
    }

    // DR7 control bits for one slot: local-enable, R/W and LEN.
    std::uint64_t hardware_control(std::uint32_t slot, std::int32_t kind, std::size_t size)
    {
        std::uint64_t rw = 0;
        if (kind == SLOPKIT_BP_HW_WRITE)
        {
            rw = 1;
        }
        else if (kind == SLOPKIT_BP_HW_READ_WRITE)
        {
            rw = 3;
        }

        std::uint64_t length = 0;
        if (size == 2)
        {
            length = 1;
        }
        else if (size == 4)
        {
            length = 3;
        }
        else if (size == 8)
        {
            length = 2;
        }
        return (std::uint64_t {1} << (2 * slot)) | (rw << (16 + 4 * slot)) | (length << (18 + 4 * slot));
    }

    // Reprograms DR0-DR3/DR7 on every seized thread, so a slot survives a stop
    // and a stale slot never outlives the session.
    slopkit_result apply_hardware(Session& session)
    {
        std::vector<slopkit::process::ProcessId>     tids;
        std::array<HardwareSlot, kHardwareSlotCount> hardware;
        {
            const std::lock_guard lock(session.debug_mutex);
            tids     = session.debug.tids;
            hardware = session.debug.hardware;
        }

        for (const auto tid : tids)
        {
            // A slot's perf attributes are derived from the DR7 value present
            // at the moment its address is written, and the kernel refuses to
            // modify an enabled breakpoint. So: disable through DR7, rewrite
            // every known address (which now lands disabled), publish the wanted
            // DR7, then rewrite the armed addresses so they pick up the enable
            // and type bits.
            if (const auto disabled = platform::set_debug_register(tid, 7, 0); !disabled)
            {
                return fail(status_for(disabled.error()), "cannot clear the debug control register");
            }
            for (std::uint32_t slot = 0; slot < kHardwareSlotCount; ++slot)
            {
                if (hardware[slot].address == 0)
                {
                    continue;
                }
                if (const auto written = platform::set_debug_register(tid, slot, hardware[slot].address); !written)
                {
                    return fail(status_for(written.error()), "cannot program a hardware breakpoint");
                }
            }

            std::uint64_t control = 0;
            for (std::uint32_t slot = 0; slot < kHardwareSlotCount; ++slot)
            {
                if (hardware[slot].armed)
                {
                    control |= hardware_control(slot, hardware[slot].kind, hardware[slot].size);
                }
            }
            if (const auto written = platform::set_debug_register(tid, 7, control); !written)
            {
                return fail(status_for(written.error()), "cannot program the debug control register");
            }
            for (std::uint32_t slot = 0; slot < kHardwareSlotCount; ++slot)
            {
                if (!hardware[slot].armed)
                {
                    continue;
                }
                if (const auto written = platform::set_debug_register(tid, slot, hardware[slot].address); !written)
                {
                    return fail(status_for(written.error()), "cannot program a hardware breakpoint");
                }
            }
        }
        return ok();
    }

    bool holds_int3(Session& session, std::uint64_t address)
    {
        std::array<std::byte, 1> byte {};
        const auto               read = session.mem.read(address, byte);
        return read && read->bytes == byte.size() && byte[0] == std::byte {0xCC};
    }

    void fill_stop(Session& session, const platform::StopStatus& status, slopkit_stop_info& out)
    {
        out.reason          = SLOPKIT_STOP_INTERRUPT;
        out.tid             = status.tid;
        out.address         = status.address;
        out.trap_address    = status.address;
        out.breakpoint_slot = status.debug_slot ? static_cast<std::int32_t>(*status.debug_slot) : -1;
        out.signal          = status.signal;

        switch (status.reason)
        {
        case platform::StopReason::interrupt:
            out.reason = SLOPKIT_STOP_INTERRUPT;
            break;
        case platform::StopReason::exited:
            out.reason = SLOPKIT_STOP_EXITED;
            break;
        case platform::StopReason::signalled:
            out.reason = SLOPKIT_STOP_SIGNALED;
            break;
        case platform::StopReason::single_step:
            out.reason = SLOPKIT_STOP_SINGLE_STEP;
            break;
        case platform::StopReason::breakpoint:
            // A hardware hit names its DR slot; otherwise it is a software int3
            // only when the byte just before RIP is still 0xCC.
            if (status.debug_slot)
            {
                out.reason       = SLOPKIT_STOP_BREAKPOINT;
                out.trap_address = status.watch_address != 0 ? status.watch_address : status.address;
            }
            else if (status.address >= 1 && holds_int3(session, status.address - 1))
            {
                out.reason       = SLOPKIT_STOP_BREAKPOINT;
                out.trap_address = status.address - 1;
            }
            else
            {
                out.reason = SLOPKIT_STOP_SINGLE_STEP;
            }
            break;
        }
    }

    slopkit_result plugin_debug_attach(void* handle, uint32_t* out_tid) noexcept
    {
        try
        {
            if (out_tid == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "null output pointer");
            }
            *out_tid      = 0;
            auto* session = lookup(handle);
            if (session == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
            }

            const std::lock_guard lock(session->debug_mutex);
            if (session->debug.attached)
            {
                *out_tid = session->debug.leader;
                return ok();
            }
            if (const auto status = platform::read_status(session->pid); status && status->tracer_pid != 0)
            {
                return fail(SLOPKIT_ERR_PERMISSION_DENIED, "the target is already traced");
            }

            std::vector<slopkit::process::ProcessId> tids;
            for (const auto& thread : platform::read_threads(session->pid))
            {
                tids.push_back(thread.tid);
            }
            if (tids.empty())
            {
                tids.push_back(session->pid);
            }

            std::size_t seized = 0;
            for (; seized < tids.size(); ++seized)
            {
                const auto result = platform::seize(tids[seized]);
                if (result)
                {
                    continue;
                }
                if (result.error() == slopkit::process::AccessError::not_found)
                {
                    // The thread went away between the listing and the seize.
                    tids.erase(tids.begin() + static_cast<std::ptrdiff_t>(seized));
                    --seized;
                    continue;
                }
                for (std::size_t i = 0; i < seized; ++i)
                {
                    (void)platform::detach(tids[i]);
                }
                return fail(status_for(result.error()), "cannot seize the target");
            }
            if (tids.empty())
            {
                return fail(SLOPKIT_ERR_NOT_FOUND, "the target has no threads");
            }

            // Interrupt and wait for each thread so the whole group is stopped
            // before any breakpoint is armed.
            for (const auto tid : tids)
            {
                const std::array<slopkit::process::ProcessId, 1> one {tid};
                if (const auto interrupted = platform::interrupt(tid); !interrupted)
                {
                    for (const auto other : tids)
                    {
                        (void)platform::detach(other);
                    }
                    return fail(status_for(interrupted.error()), "cannot stop the target");
                }
                const auto result = platform::wait(one);
                if (!result && result.error() != slopkit::process::AccessError::not_found)
                {
                    for (const auto other : tids)
                    {
                        (void)platform::detach(other);
                    }
                    return fail(status_for(result.error()), "cannot stop the target");
                }
            }

            session->debug.tids     = std::move(tids);
            session->debug.leader   = session->pid;
            session->debug.current  = session->pid;
            session->debug.attached = true;
            *out_tid                = session->debug.leader;
            log_message(
                SLOPKIT_LOG_INFO,
                std::format("debug session stopped pid {} ({} thread(s))", session->pid, session->debug.tids.size()));
            return ok();
        }
        catch (...)
        {
            return fail(SLOPKIT_ERR_INTERNAL, "unhandled exception");
        }
    }

    slopkit_result plugin_debug_detach(void* handle) noexcept
    {
        try
        {
            auto* session = lookup(handle);
            if (session == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
            }

            std::vector<slopkit::process::ProcessId>            tids;
            std::vector<std::pair<std::uint64_t, std::uint8_t>> restore;
            slopkit::process::ProcessId                         leader {};
            {
                const std::lock_guard lock(session->debug_mutex);
                if (!session->debug.attached)
                {
                    return ok();
                }
                tids   = session->debug.tids;
                leader = session->debug.leader;
                for (const auto& slot : session->debug.software)
                {
                    if (slot.armed && slot.size > 0)
                    {
                        restore.emplace_back(slot.address, static_cast<std::uint8_t>(slot.original & 0xFF));
                    }
                }
                session->debug.attached = false;
            }

            // Restore the original bytes so no int3 is left in the target, then
            // clear the slots and detach every thread.
            for (const auto& [address, original] : restore)
            {
                const std::array<std::byte, 1> bytes {static_cast<std::byte>(original)};
                (void)platform::write_bytes(leader, address, bytes);
            }

            slopkit_result result = ok();
            for (const auto tid : tids)
            {
                (void)platform::clear_debug_registers(tid);
                const auto detached = platform::detach(tid);
                if (!detached && result.code == SLOPKIT_OK)
                {
                    result = fail(status_for(detached.error()), "cannot detach the target");
                }
            }

            {
                const std::lock_guard lock(session->debug_mutex);
                session->debug.tids.clear();
                session->debug.software = {};
                session->debug.hardware = {};
            }
            log_message(SLOPKIT_LOG_INFO, std::format("debug session detached from pid {}", session->pid));
            return result;
        }
        catch (...)
        {
            return fail(SLOPKIT_ERR_INTERNAL, "unhandled exception");
        }
    }

    slopkit_result plugin_debug_continue(void*    handle,
                                         uint64_t resume_address,
                                         std::size_t /*resume_step_size*/,
                                         slopkit_stop_info* out_stop) noexcept
    {
        try
        {
            if (out_stop == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "null output pointer");
            }
            auto* session = lookup(handle);
            if (session == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
            }

            std::vector<slopkit::process::ProcessId> tids;
            slopkit::process::ProcessId              current {};
            bool                                     have_trap     = false;
            std::uint64_t                            trap_original = 0;
            {
                const std::lock_guard lock(session->debug_mutex);
                if (!session->debug.attached)
                {
                    return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "no debug session");
                }
                tids    = session->debug.tids;
                current = session->debug.current;
                if (resume_address != 0)
                {
                    for (const auto& slot : session->debug.software)
                    {
                        if (slot.armed && slot.address == resume_address)
                        {
                            have_trap     = true;
                            trap_original = slot.original;
                            break;
                        }
                    }
                }
            }

            std::expected<platform::StopStatus, slopkit::process::AccessError> stop {
                std::unexpected(slopkit::process::AccessError::internal)};

            if (resume_address != 0 && have_trap)
            {
                // Step over the trap: rewind RIP (an int3 leaves it one byte
                // past the trap), restore the original byte, single-step it and
                // write the int3 back, leaving the target stopped.
                if (const auto rewound = platform::set_register(current, "RIP", resume_address); !rewound)
                {
                    return fail(status_for(rewound.error()), "cannot rewind the trapped instruction pointer");
                }
                const std::array<std::byte, 1> original {static_cast<std::byte>(trap_original & 0xFF)};
                if (const auto restored = platform::write_bytes(current, resume_address, original); !restored)
                {
                    return fail(status_for(restored.error()), "cannot restore the trapped byte");
                }
                const std::array<slopkit::process::ProcessId, 1> one {current};
                if (const auto stepped = platform::single_step(current); !stepped)
                {
                    const std::array<std::byte, 1> trap {std::byte {0xCC}};
                    (void)platform::write_bytes(current, resume_address, trap);
                    return fail(status_for(stepped.error()), "cannot single-step the target");
                }
                stop = platform::wait(one);
                const std::array<std::byte, 1> trap {std::byte {0xCC}};
                (void)platform::write_bytes(current, resume_address, trap);
            }
            else
            {
                if (const auto hardware = apply_hardware(*session); hardware.code != SLOPKIT_OK)
                {
                    return hardware;
                }
                for (const auto tid : tids)
                {
                    (void)platform::cont(tid);
                }
                stop = platform::wait(tids);
            }

            if (!stop)
            {
                return fail(status_for(stop.error()), "the target did not report a stop");
            }
            fill_stop(*session, *stop, *out_stop);
            {
                const std::lock_guard lock(session->debug_mutex);
                session->debug.current = stop->tid;
            }
            return ok();
        }
        catch (...)
        {
            return fail(SLOPKIT_ERR_INTERNAL, "unhandled exception");
        }
    }

    slopkit_result plugin_debug_step(void* handle, uint32_t tid, slopkit_stop_info* out_stop) noexcept
    {
        try
        {
            if (out_stop == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "null output pointer");
            }
            auto* session = lookup(handle);
            if (session == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
            }
            {
                const std::lock_guard lock(session->debug_mutex);
                if (!session->debug.attached)
                {
                    return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "no debug session");
                }
            }

            const std::array<slopkit::process::ProcessId, 1> one {tid};
            if (const auto stepped = platform::single_step(tid); !stepped)
            {
                return fail(status_for(stepped.error()), "cannot single-step the target");
            }
            const auto stop = platform::wait(one);
            if (!stop)
            {
                return fail(status_for(stop.error()), "the target did not report a stop");
            }
            fill_stop(*session, *stop, *out_stop);
            const std::lock_guard lock(session->debug_mutex);
            session->debug.current = stop->tid;
            return ok();
        }
        catch (...)
        {
            return fail(SLOPKIT_ERR_INTERNAL, "unhandled exception");
        }
    }

    slopkit_result plugin_debug_interrupt(void* handle, uint32_t tid) noexcept
    {
        try
        {
            auto* session = lookup(handle);
            if (session == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
            }
            {
                const std::lock_guard lock(session->debug_mutex);
                if (!session->debug.attached)
                {
                    return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "no debug session");
                }
            }

            // Not a ptrace call: SIGSTOP is delivered to the tracee so it can be
            // sent from the worker's interrupt thread while the ptrace job thread
            // is blocked inside debug_continue's waitpid.
            const auto stopped = platform::stop_thread(session->pid, tid);
            if (!stopped && stopped.error() != slopkit::process::AccessError::not_found)
            {
                return fail(status_for(stopped.error()), "cannot interrupt the target");
            }
            return ok();
        }
        catch (...)
        {
            return fail(SLOPKIT_ERR_INTERNAL, "unhandled exception");
        }
    }

    slopkit_result plugin_debug_get_registers(void*                    handle,
                                              uint32_t                 tid,
                                              slopkit_register_value** out,
                                              std::size_t*             out_count) noexcept
    {
        try
        {
            if (out == nullptr || out_count == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "null output pointer");
            }
            *out       = nullptr;
            *out_count = 0;
            if (lookup(handle) == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
            }

            const auto registers = platform::get_registers(tid);
            if (!registers)
            {
                return fail(status_for(registers.error()), "cannot read the registers");
            }

            const std::array<std::pair<const char*, std::uint64_t>, 18> values {
                {
                 {"RAX", registers->rax},
                 {"RBX", registers->rbx},
                 {"RCX", registers->rcx},
                 {"RDX", registers->rdx},
                 {"RSI", registers->rsi},
                 {"RDI", registers->rdi},
                 {"RBP", registers->rbp},
                 {"RSP", registers->rsp},
                 {"R8", registers->r8},
                 {"R9", registers->r9},
                 {"R10", registers->r10},
                 {"R11", registers->r11},
                 {"R12", registers->r12},
                 {"R13", registers->r13},
                 {"R14", registers->r14},
                 {"R15", registers->r15},
                 {"RIP", registers->rip},
                 {"RFLAGS", registers->rflags},
                 }
            };

            auto* array =
                static_cast<slopkit_register_value*>(host_alloc(sizeof(slopkit_register_value) * values.size()));
            if (array == nullptr)
            {
                return fail(SLOPKIT_ERR_INTERNAL, "allocation failed");
            }
            for (std::size_t i = 0; i < values.size(); ++i)
            {
                array[i].name  = values[i].first;
                array[i].value = values[i].second;
            }
            *out       = array;
            *out_count = values.size();
            return ok();
        }
        catch (...)
        {
            return fail(SLOPKIT_ERR_INTERNAL, "unhandled exception");
        }
    }

    slopkit_result plugin_debug_set_register(void* handle, uint32_t tid, const char* name, uint64_t value) noexcept
    {
        try
        {
            if (name == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "null register name");
            }
            if (lookup(handle) == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
            }
            const auto written = platform::set_register(tid, name, value);
            if (!written)
            {
                return fail(status_for(written.error()), "cannot write the register");
            }
            return ok();
        }
        catch (...)
        {
            return fail(SLOPKIT_ERR_INTERNAL, "unhandled exception");
        }
    }

    slopkit_result
    plugin_debug_set_software_breakpoint(void* handle, uint32_t slot, uint64_t address, int32_t insert) noexcept
    {
        try
        {
            auto* session = lookup(handle);
            if (session == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
            }
            if (slot >= kSoftwareSlotCount)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "software slot out of range");
            }

            const std::lock_guard lock(session->debug_mutex);
            if (!session->debug.attached)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "no debug session");
            }
            auto& record = session->debug.software[slot];

            if (insert == 0)
            {
                if (record.armed && record.address == address)
                {
                    const std::array<std::byte, 1> original {static_cast<std::byte>(record.original & 0xFF)};
                    if (const auto restored = platform::write_bytes(session->debug.leader, address, original);
                        !restored)
                    {
                        return fail(status_for(restored.error()), "cannot restore the trapped byte");
                    }
                    record.armed = false;
                }
                return ok();
            }

            if (record.armed && record.address == address)
            {
                return ok();
            }
            for (std::size_t i = 0; i < kSoftwareSlotCount; ++i)
            {
                if (i != slot && session->debug.software[i].armed && session->debug.software[i].address == address)
                {
                    return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "a breakpoint already covers this address");
                }
            }

            const auto original = platform::read_bytes(session->debug.leader, address, 1);
            if (!original)
            {
                return fail(status_for(original.error()), "cannot read the breakpoint address");
            }
            if (original->empty())
            {
                return fail(SLOPKIT_ERR_IO, "cannot read the breakpoint address");
            }
            const std::array<std::byte, 1> trap {std::byte {0xCC}};
            if (const auto written = platform::write_bytes(session->debug.leader, address, trap); !written)
            {
                return fail(status_for(written.error()), "cannot arm the breakpoint");
            }
            record.armed    = true;
            record.address  = address;
            record.original = std::to_integer<std::uint8_t>((*original)[0]);
            record.size     = 1;
            return ok();
        }
        catch (...)
        {
            return fail(SLOPKIT_ERR_INTERNAL, "unhandled exception");
        }
    }

    slopkit_result plugin_debug_set_hardware_breakpoint(
        void* handle, uint32_t slot, int32_t kind, uint64_t address, std::size_t size, int32_t insert) noexcept
    {
        try
        {
            auto* session = lookup(handle);
            if (session == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
            }
            if (slot >= kHardwareSlotCount)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "hardware slot out of range");
            }
            if (kind < SLOPKIT_BP_HW_EXECUTE || kind > SLOPKIT_BP_HW_READ_WRITE)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown hardware breakpoint kind");
            }

            std::size_t effective = size;
            if (kind == SLOPKIT_BP_HW_EXECUTE)
            {
                effective = 1;
            }
            else if (effective != 1 && effective != 2 && effective != 4 && effective != 8)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "hardware breakpoint size must be 1, 2, 4 or 8");
            }

            {
                const std::lock_guard lock(session->debug_mutex);
                if (!session->debug.attached)
                {
                    return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "no debug session");
                }
                if (insert != 0)
                {
                    session->debug.hardware[slot] = HardwareSlot {true, kind, address, effective};
                }
                else
                {
                    // The address is kept so a later apply can rewrite the slot
                    // and have the kernel recompute it as disabled.
                    session->debug.hardware[slot].armed = false;
                }
            }
            return apply_hardware(*session);
        }
        catch (...)
        {
            return fail(SLOPKIT_ERR_INTERNAL, "unhandled exception");
        }
    }

    slopkit_result
    plugin_debug_backtrace(void* handle, uint32_t tid, slopkit_frame_info** out, std::size_t* out_count) noexcept
    {
        try
        {
            if (out == nullptr || out_count == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "null output pointer");
            }
            *out          = nullptr;
            *out_count    = 0;
            auto* session = lookup(handle);
            if (session == nullptr)
            {
                return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
            }

            const auto registers = platform::get_registers(tid);
            if (!registers)
            {
                return fail(status_for(registers.error()), "cannot read the registers");
            }

            std::vector<slopkit_frame_info> frames;
            frames.push_back(slopkit_frame_info {registers->rip, registers->rbp});

            constexpr std::size_t     kMaxFrames = 64;
            std::array<std::byte, 16> buffer {};
            std::uint64_t             frame_pointer = registers->rbp;
            while (frame_pointer != 0 && frames.size() < kMaxFrames)
            {
                const auto read = session->mem.read(frame_pointer, buffer);
                if (!read || read->bytes < buffer.size())
                {
                    break;
                }
                std::uint64_t saved  = 0;
                std::uint64_t caller = 0;
                for (std::size_t i = 0; i < 8; ++i)
                {
                    saved |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(buffer[i])) << (8 * i);
                    caller |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(buffer[8 + i])) << (8 * i);
                }
                // A frame pointer must grow towards the caller; a null or
                // non-advancing one ends the walk.
                if (caller == 0 || saved <= frame_pointer)
                {
                    break;
                }
                frames.push_back(slopkit_frame_info {caller, saved});
                frame_pointer = saved;
            }

            auto* array = static_cast<slopkit_frame_info*>(host_alloc(sizeof(slopkit_frame_info) * frames.size()));
            if (array == nullptr && !frames.empty())
            {
                return fail(SLOPKIT_ERR_INTERNAL, "allocation failed");
            }
            for (std::size_t i = 0; i < frames.size(); ++i)
            {
                array[i] = frames[i];
            }
            *out       = array;
            *out_count = frames.size();
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
        plugin_list_regions,
        plugin_access_methods,
        plugin_debug_attach,
        plugin_debug_detach,
        plugin_debug_continue,
        plugin_debug_step,
        plugin_debug_interrupt,
        plugin_debug_get_registers,
        plugin_debug_set_register,
        plugin_debug_set_software_breakpoint,
        plugin_debug_set_hardware_breakpoint,
        plugin_debug_backtrace,
    };
} // namespace

extern "C" SLOPKIT_PLUGIN_EXPORT const slopkit_plugin_vtable* slopkit_plugin_entry(const slopkit_host_services* host)
{
    g_host = host;
    return &g_vtable;
}
