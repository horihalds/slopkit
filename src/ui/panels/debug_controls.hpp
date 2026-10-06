#pragma once

#include <cstdint>
#include <optional>

#include <QString>
#include <QWidget>

#include "debug/controller.hpp"
#include "process/attachment.hpp"

class QPushButton;

namespace slopkit::ui::widgets
{
    class StatusLabel;
} // namespace slopkit::ui::widgets

namespace slopkit::ui::panels
{

    // The one-line debug control bar above the disassembly listing: every
    // debugger control in a single row, ending with the state read-out. It owns
    // the session-state gating, so the right-hand Debugger pane stays a pure
    // view of the registers and the call stack. Every action is submitted to the
    // controller, so the UI thread never touches the target.
    class DebugControls : public QWidget
    {
        Q_OBJECT

    public:
        DebugControls(debug::Controller& controller, const process::AttachedTarget& target, QWidget* parent = nullptr);

        // The instruction `Toggle Breakpoint` acts on: its address plus the text
        // (module+RVA or absolute) the controller resolves the address from,
        // following the listing's own rendering so the two never disagree.
        void set_selected_instruction(std::optional<std::uint64_t> address, const QString& expression);

        // Reports a rejected action in the state line, the same read-out the
        // controller's own failures use (the controller does not emit one for a
        // refused `add_breakpoint`).
        void report_error(const QString& text);

        [[nodiscard]] QPushButton*          toggle_breakpoint_button() const noexcept;
        [[nodiscard]] QPushButton*          start_button() const noexcept;
        [[nodiscard]] QPushButton*          stop_button() const noexcept;
        [[nodiscard]] QPushButton*          resume_button() const noexcept;
        [[nodiscard]] QPushButton*          break_button() const noexcept;
        [[nodiscard]] QPushButton*          step_into_button() const noexcept;
        [[nodiscard]] QPushButton*          step_over_button() const noexcept;
        [[nodiscard]] QPushButton*          step_out_button() const noexcept;
        [[nodiscard]] QPushButton*          breakpoints_button() const noexcept;
        [[nodiscard]] widgets::StatusLabel* status_label() const noexcept;

    signals:
        void breakpointsRequested();
        void toggleBreakpointRequested(std::uint64_t address, const QString& expression);

    private:
        void build_layout();
        void apply_state();
        void apply_message(debug::Controller::MessageKind kind, const QString& text);
        // Recomputes the toggle button's enablement and tooltip from the session
        // state and the selected instruction.
        void update_toggle_state();
        void start_session();

        debug::Controller&             controller_;
        const process::AttachedTarget& target_;

        QPushButton*                 toggle_breakpoint_button_ {};
        QPushButton*                 start_button_ {};
        QPushButton*                 stop_button_ {};
        QPushButton*                 resume_button_ {};
        QPushButton*                 break_button_ {};
        QPushButton*                 step_into_button_ {};
        QPushButton*                 step_over_button_ {};
        QPushButton*                 step_out_button_ {};
        QPushButton*                 breakpoints_button_ {};
        widgets::StatusLabel*        status_ {};
        std::optional<std::uint64_t> selected_address_;
        QString                      selected_expression_;
    };

} // namespace slopkit::ui::panels
