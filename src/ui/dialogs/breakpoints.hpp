#pragma once

#include <cstddef>

#include <QDialog>
#include <QString>

#include "debug/breakpoints.hpp"
#include "debug/controller.hpp"

class QPushButton;
class QTableView;

namespace slopkit::ui::models
{
    class BreakpointModel;
}

namespace slopkit::ui::widgets
{
    class ScrollingComboBox;
    class StatusLabel;
}

namespace slopkit::ui::dialogs
{
    class BreakpointsDialog : public QDialog
    {
        Q_OBJECT

    public:
        explicit BreakpointsDialog(debug::Controller& controller);

        void refresh();

        QString add_breakpoint_from_text(const QString& expression);

        [[nodiscard]] models::BreakpointModel*    model() const noexcept;
        [[nodiscard]] QTableView*                 table() const noexcept;
        [[nodiscard]] widgets::ScrollingComboBox* kind_combo() const noexcept;
        [[nodiscard]] widgets::ScrollingComboBox* size_combo() const noexcept;
        [[nodiscard]] QPushButton*                add_button() const noexcept;
        [[nodiscard]] QPushButton*                remove_button() const noexcept;
        [[nodiscard]] QPushButton*                clear_button() const noexcept;
        [[nodiscard]] widgets::StatusLabel*       hint_label() const noexcept;

        [[nodiscard]] debug::Kind selected_kind() const;
        [[nodiscard]] std::size_t selected_size() const;

    private:
        void build_layout();
        void prompt_add();
        void remove_selected();
        void update_hint();

        debug::Controller&          controller_;
        models::BreakpointModel*    model_ {};
        QTableView*                 table_ {};
        widgets::ScrollingComboBox* kind_ {};
        widgets::ScrollingComboBox* size_ {};
        QPushButton*                add_ {};
        QPushButton*                remove_ {};
        QPushButton*                clear_ {};
        widgets::StatusLabel*       hint_ {};
    };

}
