#pragma once

#include "table/address_table.hpp"

#include <QDialog>

class QCheckBox;
class QLineEdit;

namespace slopkit::ui::widgets
{
    class StatusLabel;
} // namespace slopkit::ui::widgets

namespace slopkit::ui::dialogs
{

    // The Table Settings dialog: the process the current table belongs to, the
    // auto attach toggle and the match-by-executable-path toggle. It edits the
    // table's settings in place and re-reads them whenever it is shown.
    class TableSettingsDialog : public QDialog
    {
        Q_OBJECT

    public:
        explicit TableSettingsDialog(table::AddressTable& table, QWidget* parent = nullptr);

    protected:
        void showEvent(QShowEvent* event) override;

    private:
        void commit();
        void commit_auto_attach();

        table::AddressTable& table_;

        QLineEdit*            target_edit_ {};
        QCheckBox*            auto_attach_check_ {};
        QCheckBox*            match_exe_path_check_ {};
        widgets::StatusLabel* status_ {};
        bool                  updating_ {false};
    };

} // namespace slopkit::ui::dialogs
