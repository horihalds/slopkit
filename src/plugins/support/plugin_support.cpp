#include "plugins/support/plugin_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <new>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "platform/linux/memory.hpp"
#include "platform/linux/process_control.hpp"
#include "platform/linux/procfs.hpp"
#include "plugins/support/session.hpp"

namespace slopkit::plugins::support
{
    namespace
    {
        const slopkit_host_services* g_host    = nullptr;
        const PluginProfile*         g_profile = nullptr;

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
        // plugin's id. A message must outlive the call, which a local string
        // does.
        void log_message(int32_t level, const std::string& text)
        {
            if (g_host != nullptr && g_host->log != nullptr)
            {
                g_host->log(level, text.c_str(), g_host->user_data);
            }
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

        int32_t status_for(process::AccessError error)
        {
            switch (error)
            {
            case process::AccessError::permission_denied:
                return SLOPKIT_ERR_PERMISSION_DENIED;
            case process::AccessError::not_found:
                return SLOPKIT_ERR_NOT_FOUND;
            case process::AccessError::unsupported:
                return SLOPKIT_ERR_UNSUPPORTED;
            case process::AccessError::io_error:
                return SLOPKIT_ERR_IO;
            case process::AccessError::invalid_abi:
                return SLOPKIT_ERR_INVALID_ABI;
            case process::AccessError::invalid_argument:
                return SLOPKIT_ERR_INVALID_ARGUMENT;
            case process::AccessError::internal:
                break;
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
            return g_profile->info();
        }

        int32_t plugin_precedence() noexcept
        {
            return g_profile->precedence();
        }

        uint32_t plugin_access_methods() noexcept
        {
            return g_profile->access_methods();
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

                std::vector<std::pair<process::ProcessId, std::string>> claimed;
                for (const auto pid : platform::list_pids())
                {
                    std::string name;
                    if (g_profile->claim(pid, name))
                    {
                        claimed.emplace_back(pid, std::move(name));
                    }
                }

                auto* array =
                    static_cast<slopkit_process_info*>(host_alloc(sizeof(slopkit_process_info) * claimed.size()));
                if (array == nullptr && !claimed.empty())
                {
                    return fail(SLOPKIT_ERR_INTERNAL, "allocation failed");
                }

                std::size_t count = 0;
                for (auto& [pid, name] : claimed)
                {
                    const auto exe        = platform::read_exe(pid);
                    array[count].pid      = pid;
                    array[count].name     = intern(std::move(name));
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
                new (session) Session {pid, g_profile->signal_policy};
                register_session(session);
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
                auto* session = lookup_session(handle);
                if (session == nullptr)
                {
                    return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
                }
                unregister_session(session);
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

                auto* session = lookup_session(handle);
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

                auto* session = lookup_session(handle);
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

                auto* session = lookup_session(handle);
                if (session == nullptr)
                {
                    return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
                }
                reset_arena();

                auto modules = platform::modules_from_maps(platform::read_maps(session->pid));
                platform::fill_module_entry_points(session->pid, modules);
                g_profile->shape_modules(modules, session->pid);

                auto* array =
                    static_cast<slopkit_module_info*>(host_alloc(sizeof(slopkit_module_info) * modules.size()));
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

                auto* session = lookup_session(handle);
                if (session == nullptr)
                {
                    return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
                }
                reset_arena();

                const auto threads = platform::read_threads(session->pid);
                auto*      array =
                    static_cast<slopkit_thread_info*>(host_alloc(sizeof(slopkit_thread_info) * threads.size()));
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

                auto* session = lookup_session(handle);
                if (session == nullptr)
                {
                    return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
                }
                reset_arena();

                const auto regions = platform::read_maps(session->pid);
                auto*      array =
                    static_cast<slopkit_region_info*>(host_alloc(sizeof(slopkit_region_info) * regions.size()));
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

        // --- debug operations (ABI 1.4) ------------------------------------

        void fill_stop(platform::DebugSession& debug, const platform::StopStatus& status, slopkit_stop_info& out)
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
            case platform::StopReason::signal_stop:
                out.reason = SLOPKIT_STOP_SIGNALED;
                break;
            case platform::StopReason::single_step:
                out.reason = SLOPKIT_STOP_SINGLE_STEP;
                break;
            case platform::StopReason::breakpoint:
                // A hardware hit names its DR slot; otherwise it is a software
                // int3 only when the byte just before RIP is still 0xCC.
                if (status.debug_slot)
                {
                    out.reason       = SLOPKIT_STOP_BREAKPOINT;
                    out.trap_address = status.watch_address != 0 ? status.watch_address : status.address;
                }
                else if (status.address >= 1 && debug.holds_int3(status.address - 1))
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
                auto* session = lookup_session(handle);
                if (session == nullptr)
                {
                    return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
                }

                const auto leader = session->debug.attach();
                if (!leader)
                {
                    return fail(status_for(leader.error()), "cannot attach the debugger");
                }
                *out_tid = *leader;
                log_message(SLOPKIT_LOG_INFO, std::format("debug session stopped pid {}", session->pid));
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
                auto* session = lookup_session(handle);
                if (session == nullptr)
                {
                    return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
                }

                if (const auto detached = session->debug.detach(); !detached)
                {
                    return fail(status_for(detached.error()), "cannot detach the target");
                }
                log_message(SLOPKIT_LOG_INFO, std::format("debug session detached from pid {}", session->pid));
                return ok();
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
                auto* session = lookup_session(handle);
                if (session == nullptr)
                {
                    return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
                }

                const auto stop = session->debug.resume(resume_address);
                if (!stop)
                {
                    return fail(status_for(stop.error()), "the target did not report a stop");
                }
                fill_stop(session->debug, *stop, *out_stop);
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
                auto* session = lookup_session(handle);
                if (session == nullptr)
                {
                    return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
                }
                const auto stop = session->debug.step(tid);
                if (!stop)
                {
                    return fail(status_for(stop.error()), "the target did not report a stop");
                }
                fill_stop(session->debug, *stop, *out_stop);
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
                auto* session = lookup_session(handle);
                if (session == nullptr)
                {
                    return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
                }
                if (const auto interrupted = session->debug.interrupt(tid); !interrupted)
                {
                    return fail(status_for(interrupted.error()), "cannot interrupt the target");
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
                *out          = nullptr;
                *out_count    = 0;
                auto* session = lookup_session(handle);
                if (session == nullptr)
                {
                    return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
                }

                const auto registers = session->debug.registers(tid);
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
                auto* session = lookup_session(handle);
                if (session == nullptr)
                {
                    return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
                }
                const auto written = session->debug.set_register(tid, name, value);
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
                auto* session = lookup_session(handle);
                if (session == nullptr)
                {
                    return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
                }

                if (const auto armed = session->debug.arm_software(slot, address, insert != 0); !armed)
                {
                    return fail(status_for(armed.error()), "cannot arm the breakpoint");
                }
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
                auto* session = lookup_session(handle);
                if (session == nullptr)
                {
                    return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
                }

                if (const auto armed = session->debug.arm_hardware(slot, kind, address, size, insert != 0); !armed)
                {
                    return fail(status_for(armed.error()), "cannot arm the hardware breakpoint");
                }
                return ok();
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
                auto* session = lookup_session(handle);
                if (session == nullptr)
                {
                    return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
                }

                const auto frames = session->debug.backtrace(tid);
                if (!frames)
                {
                    return fail(status_for(frames.error()), "cannot read the registers");
                }

                auto* array = static_cast<slopkit_frame_info*>(host_alloc(sizeof(slopkit_frame_info) * frames->size()));
                if (array == nullptr && !frames->empty())
                {
                    return fail(SLOPKIT_ERR_INTERNAL, "allocation failed");
                }
                for (std::size_t i = 0; i < frames->size(); ++i)
                {
                    array[i] = slopkit_frame_info {(*frames)[i].pc, (*frames)[i].frame_pointer};
                }
                *out       = array;
                *out_count = frames->size();
                return ok();
            }
            catch (...)
            {
                return fail(SLOPKIT_ERR_INTERNAL, "unhandled exception");
            }
        }

        slopkit_result plugin_suspend_target(void* handle) noexcept
        {
            try
            {
                auto* session = lookup_session(handle);
                if (session == nullptr)
                {
                    return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
                }
                const auto result = platform::suspend_process(session->pid);
                if (!result)
                {
                    return fail(status_for(result.error()), message_for(result.error()));
                }
                session->suspended = true;
                return ok();
            }
            catch (...)
            {
                return fail(SLOPKIT_ERR_INTERNAL, "unhandled exception");
            }
        }

        slopkit_result plugin_resume_target(void* handle) noexcept
        {
            try
            {
                auto* session = lookup_session(handle);
                if (session == nullptr)
                {
                    return fail(SLOPKIT_ERR_INVALID_ARGUMENT, "unknown session");
                }
                const auto result = platform::resume_process(session->pid);
                if (!result)
                {
                    return fail(status_for(result.error()), message_for(result.error()));
                }
                session->suspended = false;
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
            plugin_suspend_target,
            plugin_resume_target,
        };
    } // namespace

    const slopkit_plugin_vtable* install(const slopkit_host_services* host, const PluginProfile* profile) noexcept
    {
        g_host    = host;
        g_profile = profile;
        return &g_vtable;
    }

} // namespace slopkit::plugins::support
