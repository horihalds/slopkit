#pragma once

#include <QWidget>

#include "debug/controller.hpp"
#include "process/attachment.hpp"

class QPushButton;
class QTableView;

namespace slopkit::ui::models
{
    class CallStackModel;
    class RegisterModel;
} // namespace slopkit::ui::models

namespace slopkit::ui::widgets
{
    class StatusLabel;
} // namespace slopkit::ui::widgets

namespace slopkit::ui::panels
{

    // The Debugger pane: the run controls, the editable register table and the
    // call stack of the stopped thread. It submits every action to the
    // controller and renders what the controller reports, so the UI thread never
    // touches the target.
    class DebuggerPanel : public QWidget
    {
        Q_OBJECT

    public:
        DebuggerPanel(debug::Controller& controller, const process::AttachedTarget& target, QWidget* parent = nullptr);

        // Re-reads enablement, the state line and both tables.
        void refresh();

        [[nodiscard]] models::RegisterModel*  register_model() const noexcept;
        [[nodiscard]] models::CallStackModel* call_stack_model() const noexcept;
        [[nodiscard]] QPushButton*            start_button() const noexcept;
        [[nodiscard]] QPushButton*            stop_button() const noexcept;
        [[nodiscard]] QPushButton*            resume_button() const noexcept;
        [[nodiscard]] QPushButton*            break_button() const noexcept;
        [[nodiscard]] QPushButton*            step_into_button() const noexcept;
        [[nodiscard]] QPushButton*            step_over_button() const noexcept;
        [[nodiscard]] QPushButton*            breakpoints_button() const noexcept;
        [[nodiscard]] widgets::StatusLabel*   status_label() const noexcept;
        [[nodiscard]] QTableView*             register_table() const noexcept;
        [[nodiscard]] QTableView*             call_stack_table() const noexcept;

    signals:
        void breakpointsRequested();

    private:
        void build_layout();
        void apply_state();
        void apply_message(debug::Controller::MessageKind kind, const QString& text);
        void refresh_registers();
        void refresh_backtrace();
        void start_session();

        debug::Controller&             controller_;
        const process::AttachedTarget& target_;

        QPushButton*            start_button_ {};
        QPushButton*            stop_button_ {};
        QPushButton*            resume_button_ {};
        QPushButton*            break_button_ {};
        QPushButton*            step_into_button_ {};
        QPushButton*            step_over_button_ {};
        QPushButton*            breakpoints_button_ {};
        widgets::StatusLabel*   status_ {};
        QTableView*             register_table_ {};
        QTableView*             call_stack_table_ {};
        models::RegisterModel*  register_model_ {};
        models::CallStackModel* call_stack_model_ {};
    };

} // namespace slopkit::ui::panels
