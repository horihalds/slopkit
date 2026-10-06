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
        // A hit burst flushes once per drain, never once per hit.
        if (watch_dirty_)
        {
            watch_dirty_ = false;
            emit watchChanged();
        }
        return drained;
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
        // A stop we asked for only to install or remove a debug register: apply
        // the queued ops and keep the session running without ever reporting a
        // stop. The Stop Debugging request stays first, so it still wins.
        if (!stop_requested_ && (maintenance_stop_ || !pending_slot_ops_.empty()))
        {
            apply_pending_slot_ops();
            return;
        }

        // A stop we asked for only to read the registers once: read them and
        // resume, without ever reporting a stop or touching the listing.
        if (!stop_requested_ && capture_pending_)
        {
            capture_registers_now();
            return;
        }

        // A watch hit: record it and keep collecting, so the process is never
        // left paused for the user. `record` always consumes the stop.
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

        last_stop_ = stop;

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
                breakpoints_.count_hit(entry->id);
                emit breakpointsChanged();

                // An int3 leaves RIP one past the trap; rewind it so the
                // register view and the listing show the trapped instruction,
                // and so a following step reads the trap address immediately
                // instead of waiting for the re-read.
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
        // A stopped session already holds the register file, and no session at
        // all has nothing to read: hand the caller back right away.
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
        // The state flip is internal only: it is undone by the resume below and
        // never reaches the UI, so the panes keep showing "Running...".
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

        // The callback runs before the resume, so it sees the registers of the
        // stop; the target is put back running whether or not the read worked.
        std::function<void()> callback = std::move(capture_);
        capture_                       = nullptr;
        capture_pending_               = false;
        if (callback)
        {
            callback();
        }
        // Back to running without a signal: the UI never saw the internal stop.
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

        // The queue is drained: drop the entries whose disarm landed, then resume
        // if this was a maintenance round-trip.
        if (!pending_removals_.empty())
        {
            for (const std::uint64_t id : pending_removals_)
            {
                breakpoints_.remove(id);
            }
            pending_removals_.clear();
            emit breakpointsChanged();
        }

        if (maintenance_stop_)
        {
            maintenance_stop_ = false;
            begin_run(active_tid());
        }
    }

} // namespace slopkit::debug
