#include "debug/controller.hpp"

#include <format>
#include <utility>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "debug/step_over.hpp"

namespace slopkit::debug
{

    namespace
    {
        HardwareKind hardware_kind(Kind kind)
        {
            switch (kind)
            {
            case Kind::hardware_write:
                return HardwareKind::write;
            case Kind::hardware_read_write:
                return HardwareKind::read_write;
            case Kind::hardware_execute:
            case Kind::software:
            default:
                return HardwareKind::execute;
            }
        }
    } // namespace

    Controller::Controller(DebugBackend& backend, QObject* parent)
        : QObject(parent), backend_(backend), worker_(backend)
    {
    }

    Controller::~Controller()
    {
        if (state_ == State::running)
        {
            interrupt();
        }
    }

    void Controller::set_modules(std::vector<process::ModuleInfo> modules)
    {
        modules_.set_modules(modules);
    }

    void Controller::set_address_mode(ui::AddressMode mode) noexcept
    {
        address_mode_ = mode;
    }

    ui::AddressMode Controller::address_mode() const noexcept
    {
        return address_mode_;
    }

    const ui::ModuleSpans& Controller::modules() const noexcept
    {
        return modules_;
    }

    Controller::State Controller::state() const noexcept
    {
        return state_;
    }

    const StopEvent& Controller::last_stop() const noexcept
    {
        return last_stop_;
    }

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

    std::span<const RegisterValue> Controller::registers() const noexcept
    {
        return registers_;
    }

    std::span<const Frame> Controller::backtrace() const noexcept
    {
        return backtrace_;
    }

    process::ProcessId Controller::target() const noexcept
    {
        return pid_;
    }

    bool Controller::has_target() const noexcept
    {
        return pid_ != 0;
    }

    QString Controller::state_text() const
    {
        switch (state_)
        {
        case State::idle:
            return QStringLiteral("Not debugging");
        case State::starting:
            return QStringLiteral("Starting...");
        case State::running:
            return QStringLiteral("Running...");
        case State::stopped:
            break;
        }

        const QString address = ui::format_absolute(last_stop_.address);
        if (last_stop_.reason == StopReason::breakpoint)
        {
            const Breakpoint* entry = last_stop_.breakpoint_slot
                                        ? breakpoints_.hardware_in_slot(*last_stop_.breakpoint_slot)
                                        : breakpoints_.software_at(last_stop_.trap_address);
            if (entry != nullptr)
            {
                return QStringLiteral("Stopped at %1 (breakpoint %2)").arg(address).arg(entry->id);
            }
        }
        return QStringLiteral("Stopped at %1").arg(address);
    }

    void Controller::set_completion_hook(CompletionHook hook)
    {
        worker_.set_completion_hook(std::move(hook));
    }

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

    void Controller::start(process::ProcessId pid, std::string_view plugin_id)
    {
        if (state_ != State::idle)
        {
            notify(MessageKind::warning, QStringLiteral("A debug session is already open."));
            return;
        }
        if (pid == 0)
        {
            notify(MessageKind::warning, QStringLiteral("Attach to a target before starting the debugger."));
            return;
        }

        pid_            = pid;
        plugin_id_      = std::string(plugin_id);
        leader_         = pid;
        stop_requested_ = false;
        state_          = State::starting;
        emit stateChanged();
        notify(MessageKind::info, QStringLiteral("Starting a debug session for pid %1...").arg(pid_));
        begin_attach();
    }

    void Controller::begin_attach()
    {
        const JobId id = worker_.next_job_id();
        worker_.submit_attach(id,
                              pid_,
                              plugin_id_,
                              [this](JobResult&& result)
                              {
                                  apply_attach(std::move(result));
                              });
    }

    void Controller::apply_attach(JobResult&& result)
    {
        const auto* attach = std::get_if<AttachResult>(&result);
        if (attach == nullptr)
        {
            return;
        }
        if (attach->error)
        {
            state_ = State::idle;
            emit          stateChanged();
            const QString reason = failure_text(*attach->error);
            notify(MessageKind::error, QStringLiteral("Cannot debug pid %1: %2").arg(pid_).arg(reason));
            return;
        }
        if (stop_requested_)
        {
            stop_requested_ = false;
            state_          = State::stopped;
            end_session(QStringLiteral("Debug session closed."));
            return;
        }

        last_stop_        = StopEvent {};
        last_stop_.tid    = leader_;
        last_stop_.reason = StopReason::interrupt;
        notify(MessageKind::success, QStringLiteral("Debug session open for pid %1.").arg(pid_));
        slopkit::log::info(slopkit::log::category::debug, std::format("debug session started for pid {}", pid_));
        // The synthetic stop only gives active_tid()/interrupt() a thread to
        // name; the session itself begins running, so no refresh() and no
        // stopped() signal is emitted and the panes never flash.
        begin_run(leader_);
    }

    void Controller::stop()
    {
        if (state_ == State::idle)
        {
            return;
        }
        if (state_ == State::starting)
        {
            // The attach is still in flight; it ends the session as soon as it
            // completes.
            stop_requested_ = true;
            return;
        }
        if (state_ == State::running)
        {
            stop_requested_ = true;
            notify(MessageKind::info, QStringLiteral("Stopping the debug session..."));
            interrupt();
            return;
        }
        end_session(QStringLiteral("Debug session closed."));
    }

    void Controller::end_session(QString reason)
    {
        if (state_ == State::idle)
        {
            return;
        }
        stop_requested_ = false;
        const JobId id  = worker_.next_job_id();
        worker_.submit_detach(id,
                              [this, reason = std::move(reason)](JobResult&& result)
                              {
                                  apply_detach(std::move(result), reason);
                              });
    }

    void Controller::apply_detach(JobResult&& result, QString reason)
    {
        state_            = State::idle;
        stop_requested_   = false;
        maintenance_stop_ = false;
        pending_slot_ops_.clear();
        pending_removals_.clear();
        watch_       = AccessWatch {};
        watch_dirty_ = false;
        last_stop_   = StopEvent {};
        registers_.clear();
        backtrace_.clear();
        breakpoints_.clear();

        emit stateChanged();
        emit registersChanged();
        emit backtraceChanged();
        emit breakpointsChanged();

        const auto* detach = std::get_if<VoidResult>(&result);
        if (detach != nullptr && detach->error && detach->error != process::AccessError::not_found)
        {
            notify(MessageKind::warning, QStringLiteral("%1 (%2)").arg(reason).arg(failure_text(*detach->error)));
            return;
        }
        notify(MessageKind::info, reason);
    }

    void Controller::resume()
    {
        if (state_ != State::stopped)
        {
            return;
        }
        begin_run(active_tid());
    }

    void Controller::begin_run(std::uint32_t tid)
    {
        state_ = State::running;
        emit        stateChanged();
        const JobId id = worker_.next_job_id();
        worker_.submit_continue(id,
                                tid,
                                0,
                                0,
                                [this](JobResult&& result)
                                {
                                    apply_run_result(std::move(result));
                                });
    }

    void Controller::step_into()
    {
        step_over();
    }

    void Controller::step_over()
    {
        if (state_ != State::stopped)
        {
            return;
        }
        const std::uint64_t pc   = rip();
        const Breakpoint*   trap = pc != 0 ? breakpoints_.software_at(pc) : nullptr;
        const StepPlan      plan = plan_step(pc, trap != nullptr ? trap->address : 0, trap != nullptr ? trap->size : 1);
        const std::uint32_t tid  = active_tid();

        state_ = State::running;
        emit stateChanged();

        const JobId id = worker_.next_job_id();
        if (plan.is_plain_step())
        {
            worker_.submit_step(id,
                                tid,
                                [this](JobResult&& result)
                                {
                                    apply_run_result(std::move(result));
                                });
        }
        else
        {
            worker_.submit_continue(id,
                                    tid,
                                    plan.resume_address,
                                    plan.resume_step_size,
                                    [this](JobResult&& result)
                                    {
                                        apply_run_result(std::move(result));
                                    });
        }
    }

    void Controller::interrupt()
    {
        if (state_ != State::running)
        {
            return;
        }
        const std::uint32_t tid = active_tid();
        if (tid == 0)
        {
            return;
        }
        const JobId id = worker_.next_job_id();
        worker_.submit_interrupt(id,
                                 tid,
                                 [](JobResult&&)
                                 {
                                     // The in-flight continue reports the stop.
                                 });
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

    void Controller::refresh()
    {
        if (state_ != State::stopped)
        {
            return;
        }
        const std::uint32_t tid = active_tid();
        if (tid == 0)
        {
            return;
        }
        {
            const JobId id = worker_.next_job_id();
            worker_.submit_registers(id,
                                     tid,
                                     [this](JobResult&& result)
                                     {
                                         apply_registers(std::move(result));
                                     });
        }
        {
            const JobId id = worker_.next_job_id();
            worker_.submit_backtrace(id,
                                     tid,
                                     [this](JobResult&& result)
                                     {
                                         apply_backtrace(std::move(result));
                                     });
        }
    }

    void Controller::apply_registers(JobResult&& result)
    {
        const auto* registers = std::get_if<RegistersResult>(&result);
        if (registers == nullptr)
        {
            return;
        }
        if (registers->error)
        {
            notify(MessageKind::warning,
                   QStringLiteral("Cannot read the registers: %1").arg(failure_text(*registers->error)));
            return;
        }
        registers_ = registers->values;

        // The first read after attach is also what tells the listing where to go.
        if (last_stop_.address == 0)
        {
            last_stop_.address = rip();
            last_stop_.tid     = active_tid();
            emit stopped();
        }
        emit registersChanged();
    }

    void Controller::apply_backtrace(JobResult&& result)
    {
        const auto* frames = std::get_if<BacktraceResult>(&result);
        if (frames == nullptr)
        {
            return;
        }
        if (frames->error)
        {
            backtrace_.clear();
            emit backtraceChanged();
            return;
        }
        backtrace_ = frames->frames;
        emit backtraceChanged();
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

    const AccessWatch& Controller::watch() const noexcept
    {
        return watch_;
    }

    std::expected<void, std::string> Controller::watch_address(std::uint64_t address, Kind kind, std::size_t size)
    {
        if (state_ != State::stopped && state_ != State::running)
        {
            return std::unexpected(std::string {"Attach to a target first."});
        }

        // One watch at a time: a new one replaces the running one, rows included.
        if (watch_.state() == WatchState::watching)
        {
            slopkit::log::info(slopkit::log::category::debug,
                               std::format("replacing the access watch with 0x{:X}", address));
            stop_watch();
        }

        // A data breakpoint can only watch 1, 2, 4 or 8 bytes.
        const std::size_t bytes = size == 1 || size == 2 || size == 4 || size == 8 ? size : 4;
        auto              id    = breakpoints_.add(std::format("0x{:X}", address), address, kind, bytes);
        if (!id)
        {
            return std::unexpected(id.error());
        }
        if (Breakpoint* entry = breakpoints_.find(*id); entry != nullptr)
        {
            entry->hidden = true;
        }
        watch_.start(address, kind, bytes, *id);
        watch_dirty_ = true;
        emit watchChanged();
        emit breakpointsChanged();
        slopkit::log::info(slopkit::log::category::debug,
                           std::format("watching 0x{:X} for {} bytes of {} accesses",
                                       address,
                                       bytes,
                                       kind == Kind::hardware_read_write ? "read/write" : "write"));

        if (state_ == State::stopped)
        {
            arm(*id);
        }
        else
        {
            request_slot_op(*id, true);
        }
        return {};
    }

    void Controller::stop_watch()
    {
        if (watch_.state() == WatchState::idle)
        {
            return;
        }
        const std::uint64_t id = watch_.breakpoint_id();
        watch_.stop();
        watch_dirty_ = true;

        if (state_ == State::running)
        {
            if (const Breakpoint* entry = breakpoints_.find(id); entry != nullptr && entry->armed)
            {
                pending_removals_.push_back(id);
                request_slot_op(id, false);
            }
            else
            {
                breakpoints_.remove(id);
            }
        }
        else
        {
            if (const Breakpoint* entry = breakpoints_.find(id); entry != nullptr && entry->armed)
            {
                disarm(id);
            }
            breakpoints_.remove(id);
        }
        emit breakpointsChanged();
        emit watchChanged();
    }

    void Controller::clear_watch_hits()
    {
        watch_.clear_hits();
        emit watchChanged();
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

    void Controller::write_register(std::string_view name, std::uint64_t value)
    {
        if (state_ != State::stopped)
        {
            notify(MessageKind::warning, QStringLiteral("Registers can only be edited while the target is stopped."));
            return;
        }
        const JobId id = worker_.next_job_id();
        worker_.submit_set_register(
            id,
            active_tid(),
            std::string(name),
            value,
            [this](JobResult&& result)
            {
                const auto* written = std::get_if<VoidResult>(&result);
                if (written != nullptr && written->error)
                {
                    notify(MessageKind::warning,
                           QStringLiteral("Cannot write the register: %1").arg(failure_text(*written->error)));
                    return;
                }
                refresh();
            });
    }

    void Controller::notify(MessageKind kind, QString text)
    {
        const std::string record = text.toStdString();
        switch (kind)
        {
        case MessageKind::error:
            slopkit::log::error(slopkit::log::category::debug, record);
            break;
        case MessageKind::warning:
            slopkit::log::warning(slopkit::log::category::debug, record);
            break;
        case MessageKind::success:
            slopkit::log::info(slopkit::log::category::debug, record);
            break;
        case MessageKind::info:
            slopkit::log::debug(slopkit::log::category::debug, record);
            break;
        }
        emit message(kind, text);
    }

    std::uint64_t Controller::rip() const noexcept
    {
        for (const RegisterValue& value : registers_)
        {
            if (value.name == "RIP")
            {
                return value.value;
            }
        }
        return last_stop_.address;
    }

    std::uint32_t Controller::active_tid() const noexcept
    {
        return last_stop_.tid != 0 ? last_stop_.tid : leader_;
    }

    QString Controller::failure_text(process::AccessError error) const
    {
        switch (error)
        {
        case process::AccessError::unsupported:
            return QStringLiteral("this plugin cannot debug the target");
        case process::AccessError::permission_denied:
            return QStringLiteral("permission denied (already traced, or ptrace is restricted)");
        case process::AccessError::not_found:
            return QStringLiteral("the target is gone");
        case process::AccessError::io_error:
            return QStringLiteral("the target could not be accessed");
        case process::AccessError::invalid_argument:
            return QStringLiteral("the request was rejected");
        case process::AccessError::invalid_abi:
            return QStringLiteral("the plugin ABI does not match");
        case process::AccessError::internal:
            break;
        }
        return QStringLiteral("the operation failed");
    }

} // namespace slopkit::debug
