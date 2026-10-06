#include "debug/controller.hpp"

#include <format>
#include <utility>

#include "debug/step_over.hpp"

namespace slopkit::debug
{

    void Controller::step_out()
    {
        if (state_ != State::stopped)
        {
            return;
        }
        if (backtrace_.size() < 2 || backtrace_[1].pc == 0)
        {
            notify(MessageKind::warning, QStringLiteral("Cannot step out: the call stack has no caller frame."));
            return;
        }

        const std::uint64_t return_address = backtrace_[1].pc;

        // The user's own trap at the return address already ends the run there,
        // so it is reused instead of adding a duplicate entry.
        if (const Breakpoint* existing = breakpoints_.software_at(return_address); existing != nullptr)
        {
            if (!existing->armed)
            {
                notify(MessageKind::warning,
                       QStringLiteral("Cannot step out: the breakpoint at %1 is not armed.")
                           .arg(ui::format_absolute(return_address)));
                return;
            }
            begin_step_out_run();
            return;
        }

        const auto id = breakpoints_.add(std::format("{:X}", return_address), return_address, Kind::software, 1);
        if (!id)
        {
            notify(MessageKind::warning, QStringLiteral("Cannot step out: %1").arg(QString::fromStdString(id.error())));
            return;
        }
        breakpoints_.find(*id)->hidden = true;
        step_out_id_                   = *id;
        emit breakpointsChanged();
        submit_step_out_arm(*id);
    }

    void Controller::submit_step_out_arm(std::uint64_t id)
    {
        const Breakpoint* entry = breakpoints_.find(id);
        if (entry == nullptr)
        {
            return;
        }
        const JobId job = worker_.next_job_id();
        worker_.submit_software_breakpoint(job,
                                           entry->slot,
                                           entry->address,
                                           true,
                                           [this](JobResult&& result)
                                           {
                                               apply_step_out_arm(std::move(result));
                                           });
    }

    void Controller::disarm_step_out(const Breakpoint& entry)
    {
        // A plain disarm whose completion is ignored: the entry is already gone
        // from the table, and a failure (for example on a target that has
        // exited) must not surface as a user-facing warning.
        const JobId job = worker_.next_job_id();
        worker_.submit_software_breakpoint(job, entry.slot, entry.address, false, [](JobResult&&) {});
    }

    void Controller::apply_step_out_arm(JobResult&& result)
    {
        const auto* armed = std::get_if<VoidResult>(&result);
        if (armed == nullptr || !step_out_id_.has_value())
        {
            // A stop, or Stop Debugging, may have cancelled the step-out while
            // the insert was still in flight.
            return;
        }
        if (armed->error)
        {
            drop_step_out();
            notify(MessageKind::warning,
                   QStringLiteral("Cannot arm the step-out breakpoint: %1").arg(failure_text(*armed->error)));
            return;
        }
        if (Breakpoint* entry = breakpoints_.find(*step_out_id_); entry != nullptr)
        {
            entry->armed = true;
        }
        emit breakpointsChanged();
        begin_step_out_run();
    }

    void Controller::begin_step_out_run()
    {
        // Lift a trap sitting exactly at RIP for the resumed instruction, exactly
        // as step_over() does, so the run does not immediately re-trap on it.
        const std::uint64_t pc   = rip();
        const Breakpoint*   trap = pc != 0 ? breakpoints_.software_at(pc) : nullptr;
        const StepPlan      plan = plan_step(pc, trap != nullptr ? trap->address : 0, trap != nullptr ? trap->size : 1);

        state_ = State::running;
        emit stateChanged();

        const JobId id = worker_.next_job_id();
        worker_.submit_continue(id,
                                active_tid(),
                                plan.resume_address,
                                plan.resume_step_size,
                                [this](JobResult&& result)
                                {
                                    apply_run_result(std::move(result));
                                });
    }

    void Controller::finish_step_out()
    {
        // The transient trap was hit: disarm and erase it before the stop is
        // reported, so it never surfaces as a phantom breakpoint.
        drop_step_out();
    }

    void Controller::drop_step_out()
    {
        if (!step_out_id_.has_value())
        {
            return;
        }
        const std::uint64_t id = *step_out_id_;
        step_out_id_.reset();
        if (const Breakpoint* entry = breakpoints_.find(id); entry != nullptr && entry->armed)
        {
            disarm_step_out(*entry);
        }
        breakpoints_.remove(id);
        emit breakpointsChanged();
    }

} // namespace slopkit::debug
