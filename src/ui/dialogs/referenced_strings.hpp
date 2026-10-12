#pragma once

#include <cstdint>
#include <vector>

#include "ui/components/referenced_strings.hpp"
#include "ui/models/referenced_string_model.hpp"

#include <QDialog>
#include <QString>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;

namespace slopkit::ui::widgets
{
    class StatusLabel;
}

namespace slopkit::ui::dialogs
{
    class ReferencedStringsDialog : public QDialog
    {
        Q_OBJECT

    public:
        explicit ReferencedStringsDialog(const components::ReferencedStringTable& table, QWidget* parent = nullptr);

        void refresh();

        [[nodiscard]] QComboBox*   module_combo() const noexcept;
        [[nodiscard]] QLineEdit*   filter_edit() const noexcept;
        [[nodiscard]] QTableView*  table() const noexcept;
        [[nodiscard]] QPushButton* analyze_button() const noexcept;
        [[nodiscard]] QLabel*      empty_state() const noexcept;
        [[nodiscard]] QString      status_text() const;

        [[nodiscard]] int           row_count() const;
        [[nodiscard]] std::uint64_t instruction_at(int row) const;
        [[nodiscard]] QString       cell_text(int row, int column) const;

        void select_module(const QString& name);
        void set_filter(const QString& text);

    signals:
        void analyzeRequested();
        void instructionActivated(std::uint64_t instruction);

    private:
        void                  rebuild_rows();
        [[nodiscard]] QString selected_module() const;

        const components::ReferencedStringTable& table_;
        QComboBox*                               module_combo_ {};
        QLineEdit*                               filter_ {};
        QTableView*                              table_view_ {};
        models::ReferencedStringModel*           model_ {};
        QLabel*                                  empty_state_ {};
        widgets::StatusLabel*                    status_ {};
        QPushButton*                             analyze_ {};
    };

}
