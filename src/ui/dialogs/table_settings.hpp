#pragma once

#include <string>

#include <QDialog>

#include "process/attachment.hpp"
#include "table/address_table.hpp"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace slopkit::ui::widgets
{
    class StatusLabel;
} // namespace slopkit::ui::widgets

namespace slopkit::ui::dialogs
{

    // The Table Settings dialog: the process the current table belongs to, the
    // auto attach toggle and the match-by-executable-path toggle. It also shows
    // the currently attached process and can copy its identity into the table.
    // It edits the table's settings in place and re-reads them when shown.
    class TableSettingsDialog : public QDialog
    {
        Q_OBJECT

    public:
        TableSettingsDialog(table::AddressTable&           table,
                            const process::AttachedTarget& target,
                            QWidget*                       parent = nullptr);

        // Re-renders the attached-process card; cheap when nothing changed.
        void refresh_target();

    protected:
        void showEvent(QShowEvent* event) override;

    private:
        void commit();
        void commit_auto_attach();
        void commit_match_exe_path();
        void fill_from_target();

        table::AddressTable&           table_;
        const process::AttachedTarget& target_;

        QLineEdit*            target_edit_ {};
        QLineEdit*            exe_path_edit_ {};
        QCheckBox*            auto_attach_check_ {};
        QCheckBox*            match_exe_path_check_ {};
        QLabel*               attached_hint_ {};
        QLabel*               attached_name_ {};
        QLabel*               attached_plugin_ {};
        QLabel*               attached_exe_path_ {};
        QPushButton*          use_attached_button_ {};
        widgets::StatusLabel* status_ {};
        bool                  updating_ {false};

        // The attachment the card was last rendered from, so refresh_target()
        // can return early on the 50 ms tick when nothing changed.
        bool               rendered_ {false};
        process::ProcessId rendered_pid_ {};
        std::string        rendered_name_;
        std::string        rendered_exe_path_;
        bool               rendered_live_ {false};
    };

} // namespace slopkit::ui::dialogs
