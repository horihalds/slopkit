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
#include "debug/code_filter.hpp"
#include "debug/worker.hpp"
#include "process/types.hpp"
#include "script/symbols.hpp"
#include "ui/address_format.hpp"

namespace slopkit::debug
{
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

        explicit Controller(DebugBackend&        backend,
                            script::SymbolTable& symbols = script::default_symbol_table(),
                            QObject*             parent  = nullptr);
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

        void start(process::ProcessId pid, std::string_view plugin_id);
        void stop();
        void resume();
        void interrupt();
        void step_into();
        void step_over();
        void step_out();

        std::expected<std::uint64_t, std::string> add_breakpoint(std::string expression, Kind kind, std::size_t size);
        void                                      remove_breakpoint(std::uint64_t id);
        void                                      set_breakpoint_enabled(std::uint64_t id, bool enabled);
        void                                      clear_breakpoints();

        std::expected<void, std::string> watch_address(std::uint64_t address, Kind kind, std::size_t size);
        void                             stop_watch();
        void                             clear_watch_hits();
        [[nodiscard]] const AccessWatch& watch() const noexcept;

        void                            start_code_filter(std::vector<CodeFilterCandidate> candidates);
        void                            stop_code_filter();
        void                            filter_code_targets(bool keep_executed);
        [[nodiscard]] const CodeFilter& code_filter() const noexcept;

        void write_register(std::string_view name, std::uint64_t value);
        void refresh();

        void capture_registers(std::function<void()> on_captured);

        void        set_completion_hook(CompletionHook hook);
        std::size_t drain(std::size_t max_jobs = 32);

        // No queued job, no job in flight and no undelivered completion, so a
        // drain() would find nothing to do. What a test has to wait for before
        // asserting on state a job completion produces.
        [[nodiscard]] bool idle() const noexcept;

    signals:
        void stateChanged();
        void stopped();
        void registersChanged();
        void backtraceChanged();
        void breakpointsChanged();
        void message(slopkit::debug::Controller::MessageKind kind, const QString& text);
        void watchChanged();
        void codeFilterChanged();

    private:
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
        void resume_past_filter_trap(const StopEvent& stop);
        void stop_at_failed_filter_trap(const StopEvent& stop, std::uint64_t breakpoint_id);
        void arm_filter_candidate();
        void finish_filter_arm();
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
        script::SymbolTable&         symbols_;
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
        bool                         maintenance_stop_ {false};
        std::vector<PendingSlotOp>   pending_slot_ops_;
        std::vector<std::uint64_t>   pending_removals_;
        AccessWatch                  watch_;
        bool                         watch_dirty_ {false};
        CodeFilter                   code_filter_;
        bool                         filter_dirty_ {false};
        bool                         filter_arm_refused_ {false};
        bool                         filter_arm_pending_ {false};
        std::function<void()>        capture_;
        bool                         capture_pending_ {false};
        std::optional<std::uint64_t> step_out_id_ {};
    };

}
