#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <QObject>
#include <QString>

#include "debug/access_watch.hpp"
#include "debug/backend.hpp"
#include "debug/breakpoints.hpp"
#include "debug/worker.hpp"
#include "process/types.hpp"
#include "ui/address_format.hpp"

namespace slopkit::debug
{

    // Owns the debug session for the UI: the state machine, the session
    // breakpoint table, the register/backtrace cache and the trap-to-breakpoint
    // resolution. Every target access is submitted to the worker and applied by
    // drain() on the UI thread.
    class Controller : public QObject
    {
        Q_OBJECT

    public:
        enum class State
        {
            idle,
            starting,
            stopped,
            running,
        };

        enum class MessageKind
        {
            info,
            success,
            warning,
            error,
        };

        explicit Controller(DebugBackend& backend, QObject* parent = nullptr);
        ~Controller() override;

        Controller(const Controller&)            = delete;
        Controller& operator=(const Controller&) = delete;

        void                                 set_modules(std::vector<process::ModuleInfo> modules);
        void                                 set_address_mode(ui::AddressMode mode) noexcept;
        [[nodiscard]] ui::AddressMode        address_mode() const noexcept;
        [[nodiscard]] const ui::ModuleSpans& modules() const noexcept;

        [[nodiscard]] State                          state() const noexcept;
        [[nodiscard]] const StopEvent&               last_stop() const noexcept;
        [[nodiscard]] std::span<const Breakpoint>    breakpoints() const noexcept;
        [[nodiscard]] BreakpointTable&               table() noexcept;
        [[nodiscard]] const BreakpointTable&         table() const noexcept;
        [[nodiscard]] std::span<const RegisterValue> registers() const noexcept;
        [[nodiscard]] std::span<const Frame>         backtrace() const noexcept;
        [[nodiscard]] process::ProcessId             target() const noexcept;
        [[nodiscard]] bool                           has_target() const noexcept;
        [[nodiscard]] QString                        state_text() const;

        // Attaches and leaves the target running: only a breakpoint hit, a watch
        // hit or interrupt() stops it.
        void start(process::ProcessId pid, std::string_view plugin_id);
        void stop();
        void resume();
        void interrupt();
        void step_into();
        void step_over();
        // Runs the target out of the current function with one transient hidden
        // software breakpoint at the caller's return address. A call stack
        // without a caller frame reports a warning and changes nothing.
        void step_out();

        std::expected<std::uint64_t, std::string> add_breakpoint(std::string expression, Kind kind, std::size_t size);
        void                                      remove_breakpoint(std::uint64_t id);
        void                                      set_breakpoint_enabled(std::uint64_t id, bool enabled);
        void                                      clear_breakpoints();

        // Starts (or replaces, when one is already running) the address watch: a
        // hidden read/write hardware breakpoint on `address` that the controller
        // keeps consuming, and the target is left running for the next hit.
        std::expected<void, std::string> watch_address(std::uint64_t address, Kind kind, std::size_t size);
        void                             stop_watch();
        void                             clear_watch_hits();
        [[nodiscard]] const AccessWatch& watch() const noexcept;

        void write_register(std::string_view name, std::uint64_t value);
        void refresh();

        // Reads the register file of the running target once: the target is
        // stopped just long enough for the read and put back exactly as it was,
        // and nothing about the session (state, stop, listing position) changes.
        // `on_captured` runs on the UI thread whether or not the read produced
        // values. A stopped session or no session at all invokes it immediately.
        void capture_registers(std::function<void()> on_captured);

        void        set_completion_hook(CompletionHook hook);
        std::size_t drain(std::size_t max_jobs = 32);

    signals:
        void stateChanged();
        void stopped();
        void registersChanged();
        void backtraceChanged();
        void breakpointsChanged();
        void message(slopkit::debug::Controller::MessageKind kind, const QString& text);
        // Emitted once per drain() when hits arrived, never once per hit.
        void watchChanged();

    private:
        // One arm/disarm waiting for the invisible maintenance stop that makes a
        // debug register writable while the target runs.
        struct PendingSlotOp
        {
            std::uint64_t id {};
            bool          insert {};
        };

        void begin_run(std::uint32_t tid);
        void submit_resume(std::uint32_t tid);
        void capture_registers_now();
        void apply_capture_registers(JobResult&& result);
        void drop_capture();
        void begin_attach();
        void apply_attach(JobResult&& result);
        void apply_run_result(JobResult&& result);
        void end_session(QString reason);
        void apply_detach(JobResult&& result, QString reason);
        void apply_stop(StopEvent stop);
        void begin_step_out_run();
        void submit_step_out_arm(std::uint64_t id);
        void disarm_step_out(const Breakpoint& entry);
        void apply_step_out_arm(JobResult&& result);
        void finish_step_out();
        void drop_step_out();
        void apply_registers(JobResult&& result);
        void apply_backtrace(JobResult&& result);
        void arm(std::uint64_t id);
        void disarm(std::uint64_t id);
        void submit_arm(const Breakpoint& entry, bool insert, const QString& action);
        void mark_armed(JobResult&& result, std::uint64_t id, bool insert, const QString& action);
        void request_slot_op(std::uint64_t id, bool insert);
        void apply_pending_slot_ops();
        void notify(MessageKind kind, QString text);
        [[nodiscard]] std::uint64_t rip() const noexcept;
        [[nodiscard]] std::uint32_t active_tid() const noexcept;
        [[nodiscard]] QString       failure_text(process::AccessError error) const;

        DebugBackend&                backend_;
        Worker                       worker_;
        BreakpointTable              breakpoints_;
        ui::ModuleSpans              modules_;
        ui::AddressMode              address_mode_ {ui::AddressMode::module_relative};
        State                        state_ {State::idle};
        StopEvent                    last_stop_;
        std::vector<RegisterValue>   registers_;
        std::vector<Frame>           backtrace_;
        process::ProcessId           pid_ {};
        std::string                  plugin_id_;
        std::uint32_t                leader_ {};
        bool                         stop_requested_ {false};
        // A maintenance stop is one the controller asked for purely to install or
        // remove a debug register; it is never reported to the UI.
        bool                         maintenance_stop_ {false};
        std::vector<PendingSlotOp>   pending_slot_ops_;
        std::vector<std::uint64_t>   pending_removals_;
        AccessWatch                  watch_;
        bool                         watch_dirty_ {false};
        // A one-shot invisible register read: `capture_` runs once the stop it
        // asked for has been consumed, and `capture_pending_` marks that stop.
        std::function<void()>        capture_;
        bool                         capture_pending_ {false};
        // The transient hidden software breakpoint a step-out armed, while its
        // stop is still pending; cleared as soon as that stop is consumed or any
        // other stop cancels the step-out.
        std::optional<std::uint64_t> step_out_id_ {};
    };

} // namespace slopkit::debug
