#include "debug/controller.hpp"

#include <format>
#include <utility>

#include "core/log.hpp"
#include "core/log_categories.hpp"

namespace slopkit::debug
{
    std::size_t Controller::drain(std::size_t max_jobs)
    {
        const std::size_t drained = worker_.drain(max_jobs);
        if (watch_dirty_)
        {
            watch_dirty_ = false;
            emit watchChanged();
        }
        if (filter_dirty_)
        {
            filter_dirty_ = false;
            emit codeFilterChanged();
        }
        return drained;
    }

    bool Controller::idle() const noexcept
    {
        return worker_.idle();
    }

    void Controller::apply_run_result(JobResult&& result)
    {
        const auto* stop = std::get_if<StopResult>(&result);
        if (stop == nullptr)
        {
            return;
        }
        if (stop->error)
        {
            const QString reason = failure_text(*stop->error);
            notify(MessageKind::error,
                   QStringLiteral("The debug session ended: %1 (code %2)")
                       .arg(reason)
                       .arg(static_cast<int>(*stop->error)));
            end_session(QStringLiteral("Debug session closed."));
            return;
        }
        if (stop->stop)
        {
            apply_stop(*stop->stop);
        }
    }

    void Controller::apply_stop(StopEvent stop)
    {
        // Only the SIGSTOP the maintenance window asked for parks the whole
        // group. A breakpoint stop parks a single thread, so treating it as the
        // window would resume the group before the target has restored that
        // trap, and the thread would run the tail of the patched instruction.
        if (!stop_requested_ && stop.reason == StopReason::interrupt
            && (maintenance_stop_ || !pending_slot_ops_.empty()))
        {
            apply_pending_slot_ops();
            return;
        }

        if (!stop_requested_ && capture_pending_)
        {
            capture_registers_now();
            return;
        }

        if (!stop_requested_ && step_out_id_.has_value())
        {
            const Breakpoint* pending = breakpoints_.software_at(stop.trap_address);
            const bool own_trap = stop.reason == StopReason::breakpoint && !stop.breakpoint_slot && pending != nullptr
                               && pending->id == *step_out_id_;
            if (!own_trap)
            {
                drop_step_out();
            }
        }

        if (!stop_requested_ && stop.reason == StopReason::breakpoint && stop.breakpoint_slot.has_value()
            && watch_.state() == WatchState::watching)
        {
            const Breakpoint* entry = breakpoints_.hardware_in_slot(*stop.breakpoint_slot);
            if (entry != nullptr && entry->id == watch_.breakpoint_id())
            {
                watch_.record(stop.tid, stop.address);
                watch_dirty_ = true;
                begin_run(stop.tid);
                return;
            }
        }

        if (!stop_requested_ && stop.reason == StopReason::breakpoint && !stop.breakpoint_slot
            && code_filter_.covers(stop.trap_address))
        {
            resume_past_filter_trap(stop);
            return;
        }

        // Only the leader standing for the whole target: a worker thread that
        // exits or dies takes its tid with it, but the session lives on.
        if (!stop_requested_ && (stop.reason == StopReason::exited || stop.reason == StopReason::signalled)
            && stop.tid != leader_)
        {
            slopkit::log::debug(log::category::debug,
                                std::format("thread {} left the target ({}); the session continues",
                                            stop.tid,
                                            stop.reason == StopReason::exited ? "exited" : "killed"));
            begin_run(leader_);
            return;
        }

        last_stop_ = stop;

        if (stop_requested_ || stop.reason == StopReason::exited || stop.reason == StopReason::signalled)
        {
            const bool filter_site = stop.address >= 1 && code_filter_.covers(stop.address - 1);
            slopkit::log::info(log::category::debug,
                               std::format("session-ending stop: tid={} reason={} signal={} rip={:#x} filter-site={}",
                                           stop.tid,
                                           stop.reason == StopReason::exited      ? "exited"
                                           : stop.reason == StopReason::signalled ? "signalled"
                                                                                  : "interrupt",
                                           stop.signal,
                                           stop.address,
                                           filter_site));
        }

        if (stop_requested_)
        {
            stop_requested_ = false;
            state_          = State::stopped;
            emit stateChanged();
            end_session(QStringLiteral("Debug session closed."));
            return;
        }

        if (stop.reason == StopReason::exited)
        {
            end_session(QStringLiteral("Target exited."));
            return;
        }
        if (stop.reason == StopReason::signalled)
        {
            end_session(QStringLiteral("Target stopped by signal %1.").arg(stop.signal));
            return;
        }

        if (stop.reason == StopReason::breakpoint && !stop.breakpoint_slot)
        {
            if (const Breakpoint* entry = breakpoints_.software_at(stop.trap_address); entry != nullptr)
            {
                if (step_out_id_.has_value() && entry->id == *step_out_id_)
                {
                    finish_step_out();
                }
                else
                {
                    breakpoints_.count_hit(entry->id);
                    emit breakpointsChanged();
                }

                last_stop_.address = stop.trap_address;
                for (RegisterValue& value : registers_)
                {
                    if (value.name == "RIP")
                    {
                        value.value = stop.trap_address;
                    }
                }
                state_ = State::stopped;
                emit stateChanged();
                emit stopped();

                const JobId id = worker_.next_job_id();
                worker_.submit_set_register(id,
                                            active_tid(),
                                            "RIP",
                                            stop.trap_address,
                                            [this](JobResult&& result)
                                            {
                                                (void)result;
                                                refresh();
                                            });
                return;
            }
        }
        else if (stop.reason == StopReason::breakpoint && stop.breakpoint_slot)
        {
            if (const Breakpoint* entry = breakpoints_.hardware_in_slot(*stop.breakpoint_slot); entry != nullptr)
            {
                breakpoints_.count_hit(entry->id);
                emit breakpointsChanged();
            }
        }

        state_ = State::stopped;
        emit stateChanged();
        refresh();
        emit stopped();
    }

    void Controller::capture_registers(std::function<void()> on_captured)
    {
        if (state_ != State::running)
        {
            if (on_captured)
            {
                on_captured();
            }
            return;
        }
        capture_         = std::move(on_captured);
        capture_pending_ = true;
        interrupt();
    }

    void Controller::capture_registers_now()
    {
        state_         = State::stopped;
        const JobId id = worker_.next_job_id();
        worker_.submit_registers(id,
                                 active_tid(),
                                 [this](JobResult&& result)
                                 {
                                     apply_capture_registers(std::move(result));
                                 });
    }

    void Controller::apply_capture_registers(JobResult&& result)
    {
        if (const auto* registers = std::get_if<RegistersResult>(&result); registers != nullptr)
        {
            if (registers->error)
            {
                notify(MessageKind::warning,
                       QStringLiteral("Cannot read the registers: %1").arg(failure_text(*registers->error)));
            }
            else
            {
                registers_ = registers->values;
                emit registersChanged();
            }
        }

        std::function<void()> callback = std::move(capture_);
        capture_                       = nullptr;
        capture_pending_               = false;
        if (callback)
        {
            callback();
        }
        state_ = State::running;
        submit_resume(active_tid());
    }

    void Controller::drop_capture()
    {
        capture_         = nullptr;
        capture_pending_ = false;
    }

    void Controller::request_slot_op(std::uint64_t id, bool insert)
    {
        if (state_ != State::running)
        {
            return;
        }
        slopkit::log::debug(slopkit::log::category::debug,
                            std::format("deferring {} of breakpoint {} until the target stops for maintenance",
                                        insert ? "arm" : "disarm",
                                        id));
        pending_slot_ops_.push_back(PendingSlotOp {id, insert});
        if (!maintenance_stop_)
        {
            maintenance_stop_ = true;
            interrupt();
        }
    }

    void Controller::apply_pending_slot_ops()
    {
        if (!pending_slot_ops_.empty())
        {
            const PendingSlotOp op = pending_slot_ops_.front();
            pending_slot_ops_.erase(pending_slot_ops_.begin());

            if (const Breakpoint* entry = breakpoints_.find(op.id); entry != nullptr)
            {
                submit_arm(*entry,
                           op.insert,
                           QStringLiteral("Cannot %1 breakpoint %2").arg(op.insert ? "arm" : "disarm").arg(op.id));
            }
            else
            {
                apply_pending_slot_ops();
            }
            return;
        }

        if (!pending_removals_.empty())
        {
            for (const std::uint64_t id : pending_removals_)
            {
                breakpoints_.remove(id);
            }
            pending_removals_.clear();
            emit breakpointsChanged();
        }

        if (filter_arm_pending_)
        {
            if (code_filter_.state() != CodeFilterState::idle && !filter_arm_refused_
                && code_filter_.next_waiting().has_value())
            {
                arm_filter_candidate();
                return;
            }
            filter_arm_pending_ = false;
        }

        if (maintenance_stop_)
        {
            maintenance_stop_ = false;
            begin_run(active_tid());
        }
    }

}
