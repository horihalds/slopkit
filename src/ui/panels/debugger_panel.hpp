#pragma once

#include <QWidget>

#include "debug/controller.hpp"

class QTableView;

namespace slopkit::ui::models
{
    class CallStackModel;
    class RegisterModel;
} // namespace slopkit::ui::models

namespace slopkit::ui::panels
{

    // The Debugger pane: the editable register table and the call stack of the
    // stopped thread. The run controls and the state read-out moved to the
    // one-line DebugControls bar above the disassembly listing, so this pane is
    // a pure view of what the controller reports.
    class DebuggerPanel : public QWidget
    {
        Q_OBJECT

    public:
        explicit DebuggerPanel(debug::Controller& controller, QWidget* parent = nullptr);

        // Re-reads the register editability and both tables.
        void refresh();

        [[nodiscard]] models::RegisterModel*  register_model() const noexcept;
        [[nodiscard]] models::CallStackModel* call_stack_model() const noexcept;
        [[nodiscard]] QTableView*             register_table() const noexcept;
        [[nodiscard]] QTableView*             call_stack_table() const noexcept;

    private:
        void build_layout();
        void refresh_registers();
        void refresh_backtrace();
        // The register table is editable only while the target is stopped.
        void sync_editable();

        debug::Controller& controller_;

        QTableView*             register_table_ {};
        QTableView*             call_stack_table_ {};
        models::RegisterModel*  register_model_ {};
        models::CallStackModel* call_stack_model_ {};
    };

} // namespace slopkit::ui::panels
