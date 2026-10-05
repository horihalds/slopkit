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
        return worker_.drain(max_jobs);
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
        state_            = State::stopped;
        emit stateChanged();
        notify(MessageKind::success, QStringLiteral("Debug session open for pid %1.").arg(pid_));
        slopkit::log::info(slopkit::log::category::debug, std::format("debug session started for pid {}", pid_));
        refresh();
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
        state_          = State::idle;
        stop_requested_ = false;
        last_stop_      = StopEvent {};
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
        state_ = State::running;
        emit        stateChanged();
        const JobId id = worker_.next_job_id();
        worker_.submit_continue(id,
                                active_tid(),
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
        return *id;
    }

    void Controller::remove_breakpoint(std::uint64_t id)
    {
        if (const Breakpoint* entry = breakpoints_.find(id); entry != nullptr && entry->armed)
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

        if (state_ != State::stopped)
        {
            return;
        }
        if (enabled)
        {
            arm(id);
        }
        else
        {
            disarm(id);
        }
    }

    void Controller::clear_breakpoints()
    {
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
                                               [this, entry_id, action](JobResult&& result)
                                               {
                                                   mark_armed(std::move(result), entry_id, action);
                                               });
        }
        else
        {
            worker_.submit_software_breakpoint(id,
                                               entry.slot,
                                               entry.address,
                                               insert,
                                               [this, entry_id, action](JobResult&& result)
                                               {
                                                   mark_armed(std::move(result), entry_id, action);
                                               });
        }
    }

    void Controller::mark_armed(JobResult&& result, std::uint64_t id, const QString& action)
    {
        const auto* armed = std::get_if<VoidResult>(&result);
        if (armed == nullptr)
        {
            return;
        }
        Breakpoint* entry = breakpoints_.find(id);
        if (entry != nullptr)
        {
            entry->armed = armed->error == std::nullopt;
        }
        if (armed->error)
        {
            notify(MessageKind::warning, QStringLiteral("%1: %2").arg(action).arg(failure_text(*armed->error)));
        }
        emit breakpointsChanged();
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
