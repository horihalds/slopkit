#pragma once

#include <cstddef>
#include <optional>

#include "table/address_table.hpp"

#include <QDialog>
#include <QString>

class QKeyEvent;
class QLineEdit;
class QPushButton;

namespace slopkit::ui::components
{
    class ScriptEditor;
}

namespace slopkit::ui::widgets
{
    class StatusLabel;
}

namespace slopkit::ui::dialogs
{
    class AddScriptDialog : public QDialog
    {
        Q_OBJECT

    public:
        explicit AddScriptDialog(table::AddressTable& table, QWidget* parent = nullptr);

        void reset_for_add();
        void reset_for_add(const QString& description, const QString& source);

        void edit_entry(std::size_t row);

    protected:
        void keyPressEvent(QKeyEvent* event) override;

    private:
        void commit();
        void verify();

        table::AddressTable&       table_;
        std::optional<std::size_t> editing_;

        QLineEdit*                description_edit_ {};
        components::ScriptEditor* script_edit_ {};
        QPushButton*              commit_button_ {};
        QPushButton*              verify_button_ {};
        widgets::StatusLabel*     status_ {};
    };

}
