#pragma once

#include <QWidget>

#include "debug/controller.hpp"

class QTableView;

namespace slopkit::ui::models
{
    class CallStackModel;
    class RegisterModel;
}

namespace slopkit::ui::panels
{
    class DebuggerPanel : public QWidget
    {
        Q_OBJECT

    public:
        explicit DebuggerPanel(debug::Controller& controller, QWidget* parent = nullptr);

        void refresh();

        [[nodiscard]] models::RegisterModel*  register_model() const noexcept;
        [[nodiscard]] models::CallStackModel* call_stack_model() const noexcept;
        [[nodiscard]] QTableView*             register_table() const noexcept;
        [[nodiscard]] QTableView*             call_stack_table() const noexcept;

    signals:
        // A register edit's outcome, or why it was refused, for the main
        // window's status bar.
        void statusChanged(const QString& message, bool error);

    private:
        void build_layout();
        void refresh_registers();
        void refresh_backtrace();
        void sync_editable();

        debug::Controller& controller_;

        QTableView*             register_table_ {};
        QTableView*             call_stack_table_ {};
        models::RegisterModel*  register_model_ {};
        models::CallStackModel* call_stack_model_ {};
    };

}
