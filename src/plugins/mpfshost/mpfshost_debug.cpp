#include "plugins/mpfshost/mpfshost_debug.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "plugin/plugin_api.h"
#include "plugins/mpfshost/mpfshost_client.hpp"

namespace slopkit::plugins::mpfshost
{
    namespace
    {
        process::AccessError access_error(const ::mpfshost::Error& error)
        {
            slopkit::log::warning(slopkit::log::category::debug,
                                  std::format("mpfshost debug request failed: {}: {}",
                                              ::mpfshost::errorCodeName(error.code),
                                              message_for_error(error)));
            switch (error.code)
            {
            case ::mpfshost::ErrorCode::AccessDenied:
                return process::AccessError::permission_denied;
            case ::mpfshost::ErrorCode::TargetNotFound:
            case ::mpfshost::ErrorCode::TargetUnavailable:
            case ::mpfshost::ErrorCode::NotFound:
                return process::AccessError::not_found;
            case ::mpfshost::ErrorCode::CapabilityUnavailable:
                return process::AccessError::unsupported;
            case ::mpfshost::ErrorCode::MalformedPayload:
            case ::mpfshost::ErrorCode::UnknownMessage:
            case ::mpfshost::ErrorCode::InvalidRange:
            case ::mpfshost::ErrorCode::InvalidState:
            case ::mpfshost::ErrorCode::InvalidArgument:
                return process::AccessError::invalid_argument;
            case ::mpfshost::ErrorCode::ProtocolError:
                return process::AccessError::invalid_abi;
            default:
                return process::AccessError::io_error;
            }
        }

        struct SelectedThread
        {
            std::uint32_t vcpu {};
            std::string   thread_id;
            std::uint32_t tid {};
        };

        std::uint64_t resource_key(bool hardware, std::uint32_t slot) noexcept
        {
            return (static_cast<std::uint64_t>(hardware ? 1 : 0) << 32) | slot;
        }

        std::expected<bool, process::AccessError> ensure_paused(::mpfshost::ClientSession& session)
        {
            const auto status = session.debugState();
            if (!status)
            {
                return std::unexpected(access_error(status.error()));
            }
            if (status->runState != ::mpfshost::DebugRunState::Running)
            {
                return false;
            }
            if (const auto stopped = session.debugControl(::mpfshost::DebugControlAction::Stop); !stopped)
            {
                return std::unexpected(access_error(stopped.error()));
            }
            return true;
        }

        class ResumeAfterPause
        {
        public:
            ResumeAfterPause(::mpfshost::ClientSession& session, bool paused) : session_(session), paused_(paused) {}

            ResumeAfterPause(const ResumeAfterPause&)            = delete;
            ResumeAfterPause& operator=(const ResumeAfterPause&) = delete;

            ~ResumeAfterPause()
            {
                if (paused_)
                {
                    (void)session_.debugContinueArmed();
                }
            }

        private:
            ::mpfshost::ClientSession& session_;
            bool                       paused_ {};
        };

        std::optional<SelectedThread> select_thread(::mpfshost::ClientSession& session, std::uint32_t tid)
        {
            const auto threads = session.debugThreads();
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

        platform::StopStatus interrupt_stop(::mpfshost::ClientSession& session, std::uint32_t leader)
        {
            platform::StopStatus stop;
            stop.reason = platform::StopReason::interrupt;
            stop.tid    = leader;

            if (const auto records = session.debugRegisters(::mpfshost::kDebugAllVcpus); records)
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
        wait_for_stop(::mpfshost::ClientSession& session, std::uint32_t leader, const auto& resources)
        {
            slopkit::log::info(slopkit::log::category::debug,
                               std::format("mpfshost wait for stop with {} armed resource(s)", resources.size()));
            for (;;)
            {
                if (!resources.empty())
                {
                    std::vector<::mpfshost::DebugWaitHit> hits;
                    const auto waited = session.debugWait(1000,
                                                          1,
                                                          [&](const ::mpfshost::DebugWaitHit& hit)
                                                          {
                                                              hits.push_back(hit);
                                                              return true;
                                                          });
                    if (!waited)
                    {
                        return std::unexpected(access_error(waited.error()));
                    }
                    if (!hits.empty())
                    {
                        slopkit::log::info(
                            slopkit::log::category::debug,
                            std::format("mpfshost watch hit id={} at {:X}", hits.back().id, hits.back().address));
                        const auto&          hit = hits.back();
                        platform::StopStatus stop;
                        stop.reason       = platform::StopReason::breakpoint;
                        stop.tid          = hit.record.threadFound ? hit.record.guestTid : leader;
                        stop.address      = hit.record.rip;
                        stop.trap_address = hit.address;

                        for (const auto& [key, resource] : resources)
                        {
                            if (resource.id == hit.id)
                            {
                                // Only hardware resources carry a slot in slopkit; a software breakpoint is
                                // identified by its trap address alone.
                                if ((key >> 32) != 0)
                                {
                                    stop.debug_slot = static_cast<std::uint32_t>(key & 0xFFFFFFFFu);
                                }
                                break;
                            }
                        }
                        if (hit.kind == ::mpfshost::DebugResourceKind::Watchpoint)
                        {
                            stop.watch_address = hit.address;
                        }
                        return stop;
                    }
                }

                const auto status = session.debugState();
                if (!status)
                {
                    return std::unexpected(access_error(status.error()));
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
                if (resources.empty())
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

        const ::mpfshost::Client client {socket_path()};
        auto                     bound = client.bind(guest.target);
        if (!bound)
        {
            return std::unexpected(access_error(bound.error()));
        }

        if (const auto attached = bound->debugAttach(); !attached)
        {
            return std::unexpected(access_error(attached.error()));
        }

        auto stored       = std::make_shared<State>();
        stored->guest_pid = guest.pid;
        if (const auto threads = bound->threads(guest.pid); threads && !threads->empty())
        {
            stored->leader_tid = (*threads)[0].tid;
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
        const auto            detached = state->session->debugDetach();
        if (!detached)
        {
            return std::unexpected(access_error(detached.error()));
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
        slopkit::log::info(
            slopkit::log::category::debug,
            std::format("mpfshost resume pid={} with {} armed resource(s)", synthetic_pid, state->resources.size()));

        if (resume_address != 0 && resume_step_size != 0)
        {
            const auto selected = select_thread(*state->session, state->leader_tid);
            const auto vcpu     = selected ? selected->vcpu : 0;
            const auto thread   = selected ? selected->thread_id : std::string {};
            if (const auto stepped = state->session->debugStep(vcpu, thread); !stepped)
            {
                return std::unexpected(access_error(stepped.error()));
            }
        }

        if (const auto continued = state->session->debugContinueArmed(); !continued)
        {
            return std::unexpected(access_error(continued.error()));
        }

        // QEMU can report the stub continued while the VM is still QMP-paused. Force the VM to run before
        // waiting, or an armed watch would never see the guest execute.
        if (const auto status = state->session->debugState();
            status && status->runState != ::mpfshost::DebugRunState::Running)
        {
            (void)state->session->debugControl(::mpfshost::DebugControlAction::Continue);
        }

        return wait_for_stop(*state->session, state->leader_tid, state->resources);
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
        const auto stepped  = state->session->debugStep(vcpu, thread);
        if (!stepped)
        {
            return std::unexpected(access_error(stepped.error()));
        }

        platform::StopStatus stop;
        stop.reason  = platform::StopReason::single_step;
        stop.tid     = stepped->record.threadFound ? stepped->record.guestTid : tid;
        stop.address = stepped->record.rip;
        return stop;
    }

    std::expected<void, process::AccessError> DebugManager::interrupt(const GuestTarget& guest)
    {
        const ::mpfshost::Client client {socket_path()};
        auto                     bound = client.bind(guest.target);
        if (!bound)
        {
            return std::unexpected(access_error(bound.error()));
        }

        // The attached session polls debugState while it waits for a stop. Both requests take the host's
        // state lock, so a control request can briefly answer Busy; retry instead of dropping the interrupt.
        for (int attempt = 0; attempt < 50; ++attempt)
        {
            const auto stopped = bound->debugControl(::mpfshost::DebugControlAction::Stop);
            if (stopped)
            {
                return {};
            }
            if (stopped.error().code != ::mpfshost::ErrorCode::Busy)
            {
                return std::unexpected(access_error(stopped.error()));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds {10});
        }
        return std::unexpected(process::AccessError::io_error);
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

        // The controller captures registers after a stop, but a late resume can leave the guest running.
        // Pausing here keeps the read valid and leaves the debugger in the stopped state it is about to display.
        if (const auto paused = ensure_paused(*state->session); !paused)
        {
            return std::unexpected(paused.error());
        }

        const auto records = state->session->debugRegisters(::mpfshost::kDebugAllVcpus);
        if (!records)
        {
            return std::unexpected(access_error(records.error()));
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

    std::expected<void, process::AccessError> DebugManager::set_register(std::uint32_t    synthetic_pid,
                                                                         std::uint32_t    tid,
                                                                         std::string_view name,
                                                                         std::uint64_t    value)
    {
        auto state = state_for(synthetic_pid);
        if (!state)
        {
            return std::unexpected(process::AccessError::not_found);
        }
        const std::lock_guard lock(state->mutex);

        const auto selected = select_thread(*state->session, tid == 0 ? state->leader_tid : tid);
        if (!selected)
        {
            return std::unexpected(process::AccessError::not_found);
        }
        if (const auto written = state->session->debugSetRegister(selected->vcpu, selected->thread_id, name, value);
            !written)
        {
            return std::unexpected(access_error(written.error()));
        }
        return {};
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
        const auto            key = resource_key(false, slot);

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

        if (const auto existing = state->resources.find(key); existing != state->resources.end())
        {
            const auto& resource = existing->second;
            const auto  removed  = resource.watchpoint ? state->session->debugRemoveWatch(resource.id)
                                                       : state->session->debugRemoveBreakpoint(resource.id);
            if (!removed)
            {
                return std::unexpected(access_error(removed.error()));
            }
            state->resources.erase(existing);
        }
        if (!insert)
        {
            return {};
        }

        const auto armed = state->session->debugArmBreakpoint(
            {state->guest_pid, address, ::mpfshost::DebugBreakpointKind::Software, 30000, {}, {}});
        if (!armed)
        {
            return std::unexpected(access_error(armed.error()));
        }
        state->resources.emplace(key, Resource {armed->id, false});
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
        const auto            key = resource_key(true, slot);

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

        if (const auto existing = state->resources.find(key); existing != state->resources.end())
        {
            const auto& resource = existing->second;
            const auto  removed  = resource.watchpoint ? state->session->debugRemoveWatch(resource.id)
                                                       : state->session->debugRemoveBreakpoint(resource.id);
            if (!removed)
            {
                return std::unexpected(access_error(removed.error()));
            }
            state->resources.erase(existing);
        }
        if (!insert)
        {
            return {};
        }

        if (kind == SLOPKIT_BP_HW_EXECUTE)
        {
            const auto armed = state->session->debugArmBreakpoint(
                {state->guest_pid, address, ::mpfshost::DebugBreakpointKind::Hardware, 30000, {}, {}});
            if (!armed)
            {
                return std::unexpected(access_error(armed.error()));
            }
            state->resources.emplace(key, Resource {armed->id, false});
            return {};
        }

        const auto access =
            kind == SLOPKIT_BP_HW_WRITE ? ::mpfshost::DebugWatchAccess::Write : ::mpfshost::DebugWatchAccess::Access;
        const auto armed =
            state->session->debugArmWatch({state->guest_pid, address, watch_width(size), access, 1, 30000, {}, {}});
        if (!armed)
        {
            return std::unexpected(access_error(armed.error()));
        }
        state->resources.emplace(key, Resource {armed->id, true});
        slopkit::log::info(slopkit::log::category::debug,
                           std::format("mpfshost armed watch slot {} resource {} at {:X}", slot, armed->id, address));
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

        if (const auto paused = ensure_paused(*state->session); !paused)
        {
            return std::unexpected(paused.error());
        }

        const auto selected = select_thread(*state->session, tid == 0 ? state->leader_tid : tid);
        const auto vcpu     = selected ? selected->vcpu : 0;
        const auto thread   = selected ? selected->thread_id : std::string {};
        const auto trace    = state->session->debugBacktrace(state->guest_pid, vcpu, thread, 32);
        if (!trace)
        {
            return std::unexpected(access_error(trace.error()));
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
        const ::mpfshost::Client client {socket_path()};
        auto                     bound = client.bind(guest.target);
        if (!bound)
        {
            return std::unexpected(access_error(bound.error()));
        }
        if (const auto stopped = bound->debugControl(::mpfshost::DebugControlAction::Stop); !stopped)
        {
            return std::unexpected(access_error(stopped.error()));
        }
        return {};
    }

    std::expected<void, process::AccessError> DebugManager::resume_target(const GuestTarget& guest)
    {
        const ::mpfshost::Client client {socket_path()};
        auto                     bound = client.bind(guest.target);
        if (!bound)
        {
            return std::unexpected(access_error(bound.error()));
        }
        if (const auto continued = bound->debugControl(::mpfshost::DebugControlAction::Continue); !continued)
        {
            return std::unexpected(access_error(continued.error()));
        }
        return {};
    }

    void DebugManager::forget(std::uint32_t synthetic_pid)
    {
        const std::lock_guard lock(mutex_);
        states_.erase(synthetic_pid);
    }
}
