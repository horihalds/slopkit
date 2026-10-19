#include "debug/controller.hpp"

#include <format>
#include <utility>

#include "core/log.hpp"
#include "core/log_categories.hpp"

namespace slopkit::debug
{
    const CodeFilter& Controller::code_filter() const noexcept
    {
        return code_filter_;
    }

    void Controller::start_code_filter(std::vector<CodeFilterCandidate> candidates)
    {
        if (state_ != State::stopped)
        {
            notify(MessageKind::warning, QStringLiteral("Pause the target before collecting code targets."));
            return;
        }
        if (code_filter_.state() != CodeFilterState::idle)
        {
            stop_code_filter();
        }

        filter_arm_refused_ = false;
        code_filter_.start(std::move(candidates));
        filter_dirty_ = true;
        slopkit::log::info(log::category::debug,
                           std::format("code filter started with {} candidate(s)", code_filter_.candidates().size()));
        arm_filter_candidate();
        emit codeFilterChanged();
    }

    void Controller::stop_code_filter()
    {
        if (code_filter_.state() == CodeFilterState::idle)
        {
            return;
        }

        std::vector<std::uint64_t> ids;
        for (const CodeFilterCandidate& candidate : code_filter_.candidates())
        {
            if (candidate.visible && candidate.breakpoint_id != 0)
            {
                ids.push_back(candidate.breakpoint_id);
            }
        }
        code_filter_.reset();
        filter_dirty_       = false;
        filter_arm_refused_ = false;
        emit codeFilterChanged();
        slopkit::log::info(log::category::debug,
                           std::format("code filter cleared, {} breakpoint(s) removed", ids.size()));

        if (ids.empty())
        {
            return;
        }
        for (const std::uint64_t id : ids)
        {
            if (state_ == State::running)
            {
                Breakpoint* entry = breakpoints_.find(id);
                if (entry == nullptr)
                {
                    continue;
                }
                entry->enabled = false;
                pending_removals_.push_back(id);
                request_slot_op(id, false);
            }
            else
            {
                disarm(id);
                breakpoints_.remove(id);
            }
        }
        emit breakpointsChanged();
    }

    void Controller::filter_code_targets(bool keep_executed)
    {
        if (code_filter_.state() == CodeFilterState::idle)
        {
            return;
        }

        std::vector<std::uint64_t> ids;
        for (const CodeFilterCandidate& candidate : code_filter_.candidates())
        {
            if (!candidate.visible || candidate.breakpoint_id == 0)
            {
                continue;
            }
            if ((candidate.executions == 0) == keep_executed)
            {
                ids.push_back(candidate.breakpoint_id);
            }
        }

        const std::size_t dropped = keep_executed ? code_filter_.drop_unexecuted() : code_filter_.drop_executed();

        for (const std::uint64_t id : ids)
        {
            if (state_ == State::running)
            {
                Breakpoint* entry = breakpoints_.find(id);
                if (entry == nullptr)
                {
                    continue;
                }
                entry->enabled = false;
                pending_removals_.push_back(id);
                request_slot_op(id, false);
            }
            else
            {
                disarm(id);
                breakpoints_.remove(id);
            }
        }

        filter_dirty_ = false;
        emit codeFilterChanged();
        if (!ids.empty())
        {
            emit breakpointsChanged();
        }
        slopkit::log::debug(
            log::category::debug,
            std::format("code filter kept {} candidate(s), dropped {}", code_filter_.visible_count(), dropped));
        notify(MessageKind::info,
               QStringLiteral("%1 candidate(s) dropped, %2 left.").arg(dropped).arg(code_filter_.visible_count()));
    }

    void Controller::arm_filter_candidate()
    {
        if (code_filter_.state() == CodeFilterState::idle)
        {
            return;
        }
        if (state_ != State::stopped && state_ != State::running)
        {
            return;
        }
        const std::optional<std::uint64_t> next = code_filter_.next_waiting();
        if (!next.has_value())
        {
            return;
        }

        const std::uint64_t address = *next;
        const auto          added = breakpoints_.add(std::format("0x{:X}", address), address, Kind::software, 1, true);
        if (!added)
        {
            filter_dirty_ = true;
            if (!filter_arm_refused_)
            {
                filter_arm_refused_ = true;
                notify(MessageKind::info,
                       QStringLiteral("Cannot arm more filter breakpoints; %1 candidate(s) stay waiting.")
                           .arg(code_filter_.waiting_count()));
            }
            return;
        }

        code_filter_.mark_armed(address, *added);
        filter_dirty_ = true;

        const Breakpoint*   entry = breakpoints_.find(*added);
        const std::uint32_t slot  = entry->slot;
        const JobId         job   = worker_.next_job_id();
        worker_.submit_software_breakpoint(
            job,
            slot,
            address,
            true,
            [this, address, breakpoint_id = *added](JobResult&& result)
            {
                const auto* armed = std::get_if<VoidResult>(&result);
                if (armed == nullptr)
                {
                    return;
                }
                if (!armed->error)
                {
                    arm_filter_candidate();
                    return;
                }

                if (Breakpoint* failed = breakpoints_.find(breakpoint_id); failed != nullptr)
                {
                    failed->armed = false;
                }
                breakpoints_.remove(breakpoint_id);
                code_filter_.mark_armed(address, 0);
                filter_dirty_ = true;
                if (!filter_arm_refused_)
                {
                    filter_arm_refused_ = true;
                    notify(
                        MessageKind::warning,
                        QStringLiteral("The target refused a filter breakpoint: %1").arg(failure_text(*armed->error)));
                }
            });
    }

    void Controller::resume_past_filter_trap(const StopEvent& stop)
    {
        const Breakpoint* entry = breakpoints_.software_at(stop.trap_address);
        if (entry == nullptr)
        {
            return;
        }
        const std::uint64_t breakpoint_id = entry->id;
        const std::uint32_t slot          = entry->slot;
        const std::uint64_t address       = stop.trap_address;
        const std::uint32_t tid           = stop.tid != 0 ? stop.tid : active_tid();

        code_filter_.record(address);
        filter_dirty_ = true;
        slopkit::log::debug(log::category::debug, std::format("code-filter hit at {:#x}", address));

        // The byte is restored while the target is stopped at the trap, so the
        // plain continue that follows runs the original instruction instead of
        // re-patching the breakpoint and stepping over it.
        const JobId job = worker_.next_job_id();
        worker_.submit_software_breakpoint(job,
                                           slot,
                                           address,
                                           false,
                                           [this, stop, address, breakpoint_id, tid](JobResult&& result)
                                           {
                                               const auto* restored = std::get_if<VoidResult>(&result);
                                               if (restored == nullptr || restored->error)
                                               {
                                                   stop_at_failed_filter_trap(stop, breakpoint_id);
                                                   return;
                                               }

                                               breakpoints_.remove(breakpoint_id);
                                               filter_dirty_ = true;
                                               arm_filter_candidate();

                                               const JobId set = worker_.next_job_id();
                                               worker_.submit_set_register(set,
                                                                           tid,
                                                                           "RIP",
                                                                           address,
                                                                           [this, tid](JobResult&&)
                                                                           {
                                                                               submit_resume(tid);
                                                                           });
                                           });
    }

    void Controller::stop_at_failed_filter_trap(const StopEvent& stop, std::uint64_t breakpoint_id)
    {
        last_stop_         = stop;
        last_stop_.address = stop.trap_address;
        if (breakpoints_.find(breakpoint_id) != nullptr)
        {
            breakpoints_.count_hit(breakpoint_id);
            emit breakpointsChanged();
        }
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
        notify(MessageKind::warning,
               QStringLiteral("Cannot restore the code at %1; the target stopped.")
                   .arg(ui::format_absolute(stop.trap_address)));

        const JobId id = worker_.next_job_id();
        worker_.submit_set_register(id,
                                    active_tid(),
                                    "RIP",
                                    stop.trap_address,
                                    [this](JobResult&&)
                                    {
                                        refresh();
                                    });
    }
}
