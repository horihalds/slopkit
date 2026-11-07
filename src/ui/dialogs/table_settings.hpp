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
}

namespace slopkit::ui::dialogs
{
    class TableSettingsDialog : public QDialog
    {
        Q_OBJECT

    public:
        TableSettingsDialog(table::AddressTable&           table,
                            const process::AttachedTarget& target,
                            QWidget*                       parent = nullptr);
        ~TableSettingsDialog() override;

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

        bool               rendered_ {false};
        process::ProcessId rendered_pid_ {};
        std::string        rendered_name_;
        std::string        rendered_exe_path_;
        bool               rendered_live_ {false};
    };

}
