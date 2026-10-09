#include "debug/controller.hpp"

#include <format>
#include <utility>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "debug/step_over.hpp"

namespace slopkit::debug
{

    Controller::Controller(DebugBackend& backend, script::SymbolTable& symbols, QObject* parent)
        : QObject(parent), backend_(backend), symbols_(symbols), worker_(backend)
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
        drop_capture();
        const JobId id = worker_.next_job_id();
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
        drop_capture();
        step_out_id_.reset();
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
        emit stateChanged();
        submit_resume(tid);
        // A capture whose stop was consumed by a maintenance round-trip would
        // otherwise never get its registers read, so the stop is asked for
        // again now that the session runs.
        if (capture_pending_)
        {
            interrupt();
        }
    }

    void Controller::submit_resume(std::uint32_t tid)
    {
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
