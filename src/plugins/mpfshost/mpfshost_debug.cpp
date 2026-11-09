#include "plugins/mpfshost/mpfshost_debug.hpp"

#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "core/log.hpp"
#include "plugin/plugin_api.h"

namespace slopkit::plugins::mpfshost
{
    namespace
    {
        process::AccessError access_error(std::int32_t status) noexcept
        {
            switch (status)
            {
            case SLOPKIT_ERR_PERMISSION_DENIED:
                return process::AccessError::permission_denied;
            case SLOPKIT_ERR_NOT_FOUND:
                return process::AccessError::not_found;
            case SLOPKIT_ERR_UNSUPPORTED:
                return process::AccessError::unsupported;
            case SLOPKIT_ERR_INVALID_ARGUMENT:
                return process::AccessError::invalid_argument;
            case SLOPKIT_ERR_INVALID_ABI:
                return process::AccessError::invalid_abi;
            case SLOPKIT_ERR_IO:
                return process::AccessError::io_error;
            default:
                return process::AccessError::internal;
            }
        }

        struct SelectedThread
        {
            std::uint32_t vcpu {};
            std::string   thread_id;
            std::uint32_t tid {};
        };

        std::expected<bool, process::AccessError> ensure_paused(ClientSession& session)
        {
            const auto status = session.debug_state();
            if (!status)
            {
                return std::unexpected(access_error(status.error().code));
            }
            if (status->runState != ::mpfshost::DebugRunState::Running)
            {
                return false;
            }
            if (const auto stopped = session.debug_control(::mpfshost::DebugControlAction::Stop); !stopped)
            {
                return std::unexpected(access_error(stopped.error().code));
            }
            return true;
        }

        class ResumeAfterPause
        {
        public:
            ResumeAfterPause(ClientSession& session, bool paused) : session_(session), paused_(paused) {}

            ResumeAfterPause(const ResumeAfterPause&)            = delete;
            ResumeAfterPause& operator=(const ResumeAfterPause&) = delete;

            ~ResumeAfterPause()
            {
                if (paused_)
                {
                    (void)session_.debug_continue_armed();
                }
            }

        private:
            ClientSession& session_;
            bool           paused_ {};
        };

        std::optional<SelectedThread> select_thread(ClientSession& session, std::uint32_t tid)
        {
            const auto threads = session.debug_threads();
            if (!threads || threads->empty())
            {
                return std::nullopt;
            }

            if (tid != 0)
            {
                for (const auto& thread : *threads)
                {
                    if (thread.threadFound && thread.tid == tid)
                    {
                        return SelectedThread {thread.vcpu, thread.threadId, thread.tid};
                    }
                }
            }

            for (const auto& thread : *threads)
            {
                if (tid == 0 || thread.threadFound)
                {
                    return SelectedThread {thread.vcpu, thread.threadId, thread.tid};
                }
            }
            return SelectedThread {threads->front().vcpu, threads->front().threadId, tid};
        }

        std::uint32_t watch_width(std::size_t size) noexcept
        {
            switch (size)
            {
            case 1:
            case 2:
            case 4:
            case 8:
                return static_cast<std::uint32_t>(size);
            default:
                return 4;
            }
        }

        platform::StopStatus interrupt_stop(ClientSession& session, std::uint32_t leader)
        {
            platform::StopStatus stop;
            stop.reason = platform::StopReason::interrupt;
            stop.tid    = leader;

            if (const auto records = session.debug_registers(::mpfshost::kDebugAllVcpus); records)
            {
                const ::mpfshost::DebugRegisterRecord* selected = nullptr;
                for (const auto& record : *records)
                {
                    if (leader != 0 && record.threadFound && record.guestTid == leader)
                    {
                        selected = &record;
                        break;
                    }
                }
                if (selected == nullptr && !records->empty())
                {
                    selected = &records->front();
                }
                if (selected != nullptr)
                {
                    stop.address = selected->rip;
                    if (selected->threadFound)
                    {
                        stop.tid = selected->guestTid;
                    }
                }
            }
            return stop;
        }

        std::expected<platform::StopStatus, process::AccessError>
        wait_for_stop(ClientSession& session, std::uint32_t leader, bool has_resources)
        {
            for (;;)
            {
                if (has_resources)
                {
                    const auto waited = session.debug_wait(1000, 1);
                    if (waited)
                    {
                        if (!waited->hits.empty())
                        {
                            const auto&          hit = waited->hits.back();
                            platform::StopStatus stop;
                            stop.reason       = platform::StopReason::breakpoint;
                            stop.tid          = hit.record.threadFound ? hit.record.guestTid : leader;
                            stop.address      = hit.record.rip;
                            stop.trap_address = hit.address;
                            stop.watch_address =
                                hit.kind == ::mpfshost::DebugResourceKind::Watchpoint ? hit.address : 0;
                            return stop;
                        }
                    }
                }

                const auto status = session.debug_state();
                if (!status)
                {
                    return std::unexpected(access_error(status.error().code));
                }
                if (status->runState == ::mpfshost::DebugRunState::Shutdown)
                {
                    platform::StopStatus stop;
                    stop.reason = platform::StopReason::exited;
                    stop.tid    = leader;
                    return stop;
                }
                if (status->hostPaused || status->runState == ::mpfshost::DebugRunState::Paused)
                {
                    return interrupt_stop(session, leader);
                }
                if (!has_resources)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds {50});
                }
            }
        }
    }

    std::expected<process::ProcessId, process::AccessError> DebugManager::attach(const GuestTarget& guest,
                                                                                 std::uint32_t      synthetic_pid)
    {
        const std::lock_guard lock(mutex_);
        if (states_.contains(synthetic_pid))
        {
            return std::unexpected(process::AccessError::internal);
        }

        const Client client {socket_path()};
        auto         bound = client.bind(guest.target);
        if (!bound)
        {
            return std::unexpected(access_error(bound.error().code));
        }

        if (const auto attached = bound->debug_attach(); !attached)
        {
            return std::unexpected(access_error(attached.error().code));
        }

        auto stored       = std::make_shared<State>();
        stored->guest_pid = guest.pid;
        if (const auto threads = bound->threads(guest.pid); threads && !threads->empty())
        {
            stored->leader_tid = threads->front().tid;
        }
        stored->session   = std::move(*bound);
        const auto leader = stored->leader_tid;
        states_.emplace(synthetic_pid, std::move(stored));
        return leader;
    }

    std::expected<void, process::AccessError> DebugManager::detach(std::uint32_t synthetic_pid)
    {
        std::shared_ptr<State> state;
        {
            const std::lock_guard lock(mutex_);
            const auto            found = states_.find(synthetic_pid);
            if (found == states_.end())
            {
                return {};
            }
            state = found->second;
            states_.erase(found);
        }
        if (!state || !state->session)
        {
            return {};
        }

        const std::lock_guard lock(state->mutex);
        const auto            detached = state->session->debug_detach();
        if (!detached)
        {
            return std::unexpected(access_error(detached.error().code));
        }
        return {};
    }

    std::shared_ptr<DebugManager::State> DebugManager::state_for(std::uint32_t synthetic_pid)
    {
        const std::lock_guard lock(mutex_);
        const auto            found = states_.find(synthetic_pid);
        return found == states_.end() || !found->second->session ? nullptr : found->second;
    }

    std::expected<platform::StopStatus, process::AccessError>
    DebugManager::resume(std::uint32_t synthetic_pid, std::uint64_t resume_address, std::size_t resume_step_size)
    {
        auto state = state_for(synthetic_pid);
        if (!state)
        {
            return std::unexpected(process::AccessError::not_found);
        }
        const std::lock_guard lock(state->mutex);

        if (resume_address != 0 && resume_step_size != 0)
        {
            const auto selected = select_thread(*state->session, state->leader_tid);
            const auto vcpu     = selected ? selected->vcpu : 0;
            const auto thread   = selected ? selected->thread_id : std::string {};
            if (const auto stepped = state->session->debug_step(vcpu, thread); !stepped)
            {
                return std::unexpected(access_error(stepped.error().code));
            }
        }

        if (const auto continued = state->session->debug_continue_armed(); !continued)
        {
            return std::unexpected(access_error(continued.error().code));
        }

        return wait_for_stop(*state->session, state->leader_tid, !state->resources.empty());
    }

    std::expected<platform::StopStatus, process::AccessError> DebugManager::step(std::uint32_t synthetic_pid,
                                                                                 std::uint32_t tid)
    {
        auto state = state_for(synthetic_pid);
        if (!state)
        {
            return std::unexpected(process::AccessError::not_found);
        }
        const std::lock_guard lock(state->mutex);

        const auto selected = select_thread(*state->session, tid == 0 ? state->leader_tid : tid);
        const auto vcpu     = selected ? selected->vcpu : 0;
        const auto thread   = selected ? selected->thread_id : std::string {};
        const auto stepped  = state->session->debug_step(vcpu, thread);
        if (!stepped)
        {
            return std::unexpected(access_error(stepped.error().code));
        }

        platform::StopStatus stop;
        stop.reason  = platform::StopReason::single_step;
        stop.tid     = stepped->record.threadFound ? stepped->record.guestTid : tid;
        stop.address = stepped->record.rip;
        return stop;
    }

    std::expected<void, process::AccessError> DebugManager::interrupt(const GuestTarget& guest)
    {
        const Client client {socket_path()};
        auto         bound = client.bind(guest.target);
        if (!bound)
        {
            return std::unexpected(access_error(bound.error().code));
        }
        if (const auto stopped = bound->debug_control(::mpfshost::DebugControlAction::Stop); !stopped)
        {
            return std::unexpected(access_error(stopped.error().code));
        }
        return {};
    }

    std::expected<platform::Registers, process::AccessError> DebugManager::registers(std::uint32_t synthetic_pid,
                                                                                     std::uint32_t tid)
    {
        auto state = state_for(synthetic_pid);
        if (!state)
        {
            return std::unexpected(process::AccessError::not_found);
        }
        const std::lock_guard lock(state->mutex);

        const auto records = state->session->debug_registers(::mpfshost::kDebugAllVcpus);
        if (!records)
        {
            return std::unexpected(access_error(records.error().code));
        }

        const ::mpfshost::DebugRegisterRecord* selected = nullptr;
        for (const auto& record : *records)
        {
            if (tid != 0 && record.threadFound && record.guestTid == tid)
            {
                selected = &record;
                break;
            }
        }
        if (selected == nullptr && !records->empty())
        {
            selected = &records->front();
        }
        if (selected == nullptr)
        {
            return std::unexpected(process::AccessError::not_found);
        }

        platform::Registers result;
        result.rax    = selected->rax;
        result.rbx    = selected->rbx;
        result.rcx    = selected->rcx;
        result.rdx    = selected->rdx;
        result.rsi    = selected->rsi;
        result.rdi    = selected->rdi;
        result.rbp    = selected->rbp;
        result.rsp    = selected->rsp;
        result.r8     = selected->r8;
        result.r9     = selected->r9;
        result.r10    = selected->r10;
        result.r11    = selected->r11;
        result.r12    = selected->r12;
        result.r13    = selected->r13;
        result.r14    = selected->r14;
        result.r15    = selected->r15;
        result.rip    = selected->rip;
        result.rflags = selected->eflags;
        return result;
    }

    std::expected<void, process::AccessError>
    DebugManager::set_register(std::uint32_t, std::uint32_t, std::string_view, std::uint64_t)
    {
        return std::unexpected(process::AccessError::unsupported);
    }

    std::expected<void, process::AccessError>
    DebugManager::arm_software(std::uint32_t synthetic_pid, std::uint32_t slot, std::uint64_t address, bool insert)
    {
        auto state = state_for(synthetic_pid);
        if (!state)
        {
            return std::unexpected(process::AccessError::not_found);
        }
        const std::lock_guard lock(state->mutex);

        [[maybe_unused]] std::optional<ResumeAfterPause> resume;
        if (insert)
        {
            const auto paused = ensure_paused(*state->session);
            if (!paused)
            {
                return std::unexpected(paused.error());
            }
            resume.emplace(*state->session, *paused);
        }

        if (const auto existing = state->resources.find(slot); existing != state->resources.end())
        {
            const auto& resource = existing->second;
            const auto  removed  = resource.watchpoint ? state->session->debug_remove_watch(resource.id)
                                                       : state->session->debug_remove_breakpoint(resource.id);
            if (!removed)
            {
                return std::unexpected(access_error(removed.error().code));
            }
            state->resources.erase(existing);
        }
        if (!insert)
        {
            return {};
        }

        const auto armed =
            state->session->debug_arm_breakpoint(state->guest_pid, address, ::mpfshost::DebugBreakpointKind::Software);
        if (!armed)
        {
            return std::unexpected(access_error(armed.error().code));
        }
        state->resources.emplace(slot, Resource {armed->id, false});
        return {};
    }

    std::expected<void, process::AccessError> DebugManager::arm_hardware(std::uint32_t synthetic_pid,
                                                                         std::uint32_t slot,
                                                                         std::int32_t  kind,
                                                                         std::uint64_t address,
                                                                         std::size_t   size,
                                                                         bool          insert)
    {
        auto state = state_for(synthetic_pid);
        if (!state)
        {
            return std::unexpected(process::AccessError::not_found);
        }
        const std::lock_guard lock(state->mutex);

        [[maybe_unused]] std::optional<ResumeAfterPause> resume;
        if (insert)
        {
            const auto paused = ensure_paused(*state->session);
            if (!paused)
            {
                return std::unexpected(paused.error());
            }
            resume.emplace(*state->session, *paused);
        }

        if (const auto existing = state->resources.find(slot); existing != state->resources.end())
        {
            const auto& resource = existing->second;
            const auto  removed  = resource.watchpoint ? state->session->debug_remove_watch(resource.id)
                                                       : state->session->debug_remove_breakpoint(resource.id);
            if (!removed)
            {
                return std::unexpected(access_error(removed.error().code));
            }
            state->resources.erase(existing);
        }
        if (!insert)
        {
            return {};
        }

        if (kind == SLOPKIT_BP_HW_EXECUTE)
        {
            const auto armed = state->session->debug_arm_breakpoint(
                state->guest_pid, address, ::mpfshost::DebugBreakpointKind::Hardware);
            if (!armed)
            {
                return std::unexpected(access_error(armed.error().code));
            }
            state->resources.emplace(slot, Resource {armed->id, false});
            return {};
        }

        const auto access =
            kind == SLOPKIT_BP_HW_WRITE ? ::mpfshost::DebugWatchAccess::Write : ::mpfshost::DebugWatchAccess::Access;
        const auto armed = state->session->debug_arm_watch(state->guest_pid, address, watch_width(size), access);
        if (!armed)
        {
            return std::unexpected(access_error(armed.error().code));
        }
        state->resources.emplace(slot, Resource {armed->id, true});
        return {};
    }

    std::expected<std::vector<platform::DebugFrame>, process::AccessError>
    DebugManager::backtrace(std::uint32_t synthetic_pid, std::uint32_t tid)
    {
        auto state = state_for(synthetic_pid);
        if (!state)
        {
            return std::unexpected(process::AccessError::not_found);
        }
        const std::lock_guard lock(state->mutex);

        const auto selected = select_thread(*state->session, tid == 0 ? state->leader_tid : tid);
        const auto vcpu     = selected ? selected->vcpu : 0;
        const auto thread   = selected ? selected->thread_id : std::string {};
        const auto trace    = state->session->debug_backtrace(state->guest_pid, vcpu, thread, 32);
        if (!trace)
        {
            return std::unexpected(access_error(trace.error().code));
        }

        std::vector<platform::DebugFrame> frames;
        frames.reserve(trace->frames.size());
        for (const auto& frame : trace->frames)
        {
            frames.push_back(platform::DebugFrame {frame.address, frame.framePointer});
        }
        return frames;
    }

    std::expected<void, process::AccessError> DebugManager::suspend(const GuestTarget& guest)
    {
        const Client client {socket_path()};
        auto         bound = client.bind(guest.target);
        if (!bound)
        {
            return std::unexpected(access_error(bound.error().code));
        }
        if (const auto stopped = bound->debug_control(::mpfshost::DebugControlAction::Stop); !stopped)
        {
            return std::unexpected(access_error(stopped.error().code));
        }
        return {};
    }

    std::expected<void, process::AccessError> DebugManager::resume_target(const GuestTarget& guest)
    {
        const Client client {socket_path()};
        auto         bound = client.bind(guest.target);
        if (!bound)
        {
            return std::unexpected(access_error(bound.error().code));
        }
        if (const auto continued = bound->debug_control(::mpfshost::DebugControlAction::Continue); !continued)
        {
            return std::unexpected(access_error(continued.error().code));
        }
        return {};
    }

    void DebugManager::forget(std::uint32_t synthetic_pid)
    {
        const std::lock_guard lock(mutex_);
        states_.erase(synthetic_pid);
    }
}
