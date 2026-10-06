#include "debug/controller.hpp"

#include <utility>

namespace slopkit::debug
{

    std::span<const Breakpoint> Controller::breakpoints() const noexcept
    {
        return breakpoints_.entries();
    }

    BreakpointTable& Controller::table() noexcept
    {
        return breakpoints_;
    }

    const BreakpointTable& Controller::table() const noexcept
    {
        return breakpoints_;
    }

    std::expected<std::uint64_t, std::string>
    Controller::add_breakpoint(std::string expression, Kind kind, std::size_t size)
    {
        const auto address = ui::parse_address_text(expression, modules_);
        if (!address)
        {
            return std::unexpected(std::string {"cannot resolve the address"});
        }
        auto id = breakpoints_.add(expression, *address, kind, size);
        if (!id)
        {
            return std::unexpected(id.error());
        }
        emit breakpointsChanged();
        if (state_ == State::stopped)
        {
            arm(*id);
        }
        else if (state_ == State::running)
        {
            request_slot_op(*id, true);
        }
        return *id;
    }

    void Controller::remove_breakpoint(std::uint64_t id)
    {
        Breakpoint* entry = breakpoints_.find(id);
        if (entry == nullptr)
        {
            return;
        }
        if (entry->hidden && watch_.breakpoint_id() == id)
        {
            watch_.stop();
            watch_dirty_ = true;
        }
        if (state_ == State::running)
        {
            // The entry must outlive the queued disarm, so it is erased once the
            // maintenance round-trip drains instead of here.
            entry->enabled = false;
            if (entry->armed)
            {
                pending_removals_.push_back(id);
                request_slot_op(id, false);
            }
            else
            {
                breakpoints_.remove(id);
            }
            emit breakpointsChanged();
            return;
        }
        if (entry->armed)
        {
            disarm(id);
        }
        breakpoints_.remove(id);
        emit breakpointsChanged();
    }

    void Controller::set_breakpoint_enabled(std::uint64_t id, bool enabled)
    {
        Breakpoint* entry = breakpoints_.find(id);
        if (entry == nullptr)
        {
            return;
        }
        entry->enabled = enabled;
        emit breakpointsChanged();

        if (state_ == State::stopped)
        {
            if (enabled)
            {
                arm(id);
            }
            else
            {
                disarm(id);
            }
        }
        else if (state_ == State::running && entry->armed != enabled)
        {
            request_slot_op(id, enabled);
        }
    }

    void Controller::clear_breakpoints()
    {
        if (watch_.state() == WatchState::watching)
        {
            watch_.stop();
            watch_dirty_ = true;
        }
        if (state_ == State::running)
        {
            const std::vector<Breakpoint> entries(breakpoints_.entries().begin(), breakpoints_.entries().end());
            for (const Breakpoint& entry : entries)
            {
                if (entry.armed)
                {
                    pending_removals_.push_back(entry.id);
                    request_slot_op(entry.id, false);
                }
                else
                {
                    breakpoints_.remove(entry.id);
                }
            }
            emit breakpointsChanged();
            return;
        }
        for (const Breakpoint& entry : breakpoints_.entries())
        {
            if (entry.armed)
            {
                disarm(entry.id);
            }
        }
        breakpoints_.clear();
        emit breakpointsChanged();
    }

    void Controller::arm(std::uint64_t id)
    {
        const Breakpoint* entry = breakpoints_.find(id);
        if (entry == nullptr || !entry->enabled)
        {
            return;
        }
        submit_arm(*entry, true, QStringLiteral("Cannot arm breakpoint %1").arg(id));
    }

    void Controller::disarm(std::uint64_t id)
    {
        const Breakpoint* entry = breakpoints_.find(id);
        if (entry == nullptr || !entry->armed)
        {
            return;
        }
        submit_arm(*entry, false, QStringLiteral("Cannot disarm breakpoint %1").arg(id));
    }

    void Controller::submit_arm(const Breakpoint& entry, bool insert, const QString& action)
    {
        const JobId         id       = worker_.next_job_id();
        const std::uint64_t entry_id = entry.id;

        if (is_hardware(entry.kind))
        {
            worker_.submit_hardware_breakpoint(id,
                                               entry.slot,
                                               hardware_kind(entry.kind),
                                               entry.address,
                                               entry.size,
                                               insert,
                                               [this, entry_id, insert, action](JobResult&& result)
                                               {
                                                   mark_armed(std::move(result), entry_id, insert, action);
                                               });
        }
        else
        {
            worker_.submit_software_breakpoint(id,
                                               entry.slot,
                                               entry.address,
                                               insert,
                                               [this, entry_id, insert, action](JobResult&& result)
                                               {
                                                   mark_armed(std::move(result), entry_id, insert, action);
                                               });
        }
    }

    void Controller::mark_armed(JobResult&& result, std::uint64_t id, bool insert, const QString& action)
    {
        const auto* armed = std::get_if<VoidResult>(&result);
        if (armed == nullptr)
        {
            return;
        }
        Breakpoint* entry = breakpoints_.find(id);
        if (entry != nullptr)
        {
            entry->armed = insert && !armed->error;
        }
        if (armed->error)
        {
            notify(MessageKind::warning, QStringLiteral("%1: %2").arg(action).arg(failure_text(*armed->error)));
        }
        // Only the current watch's slot feeds the watch; a stale completion for a
        // replaced or removed entry is ignored.
        if (entry != nullptr && entry->hidden && watch_.breakpoint_id() == id)
        {
            watch_.mark_armed(entry->armed);
            watch_dirty_ = true;
        }
        emit breakpointsChanged();

        // A maintenance round-trip chains the next queued op, and resumes only
        // once the whole queue is applied.
        if (maintenance_stop_)
        {
            apply_pending_slot_ops();
        }
    }

} // namespace slopkit::debug
