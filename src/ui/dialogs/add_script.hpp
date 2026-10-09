#pragma once

#include <cstddef>
#include <optional>

#include "table/address_table.hpp"

#include <QDialog>
#include <QString>

class QLineEdit;
class QPushButton;

namespace slopkit::ui::components
{
    class ScriptEditor;
} // namespace slopkit::ui::components

namespace slopkit::ui::widgets
{
    class StatusLabel;
} // namespace slopkit::ui::widgets

namespace slopkit::ui::dialogs
{

    // The Add/Edit Script dialog: a description field and a multi-line
    // monospace editor. Committing appends a script entry, or replaces the
    // description and the source of the script entry it was opened on. The
    // owner chooses the mode with reset_for_add() or edit_entry() before showing
    // the dialog.
    class AddScriptDialog : public QDialog
    {
        Q_OBJECT

    public:
        explicit AddScriptDialog(table::AddressTable& table, QWidget* parent = nullptr);

        // Prepares the dialog for appending: a clean form in add mode.
        void reset_for_add();

        // Prepares the dialog for editing `row`: its description and source are
        // shown and committing replaces them. A row that is not a script entry
        // leaves the dialog in add mode.
        void edit_entry(std::size_t row);

    private:
        void commit();
        // Compiles the editor's text without running it and reports the verdict.
        void verify();

        table::AddressTable&       table_;
        std::optional<std::size_t> editing_;

        QLineEdit*                description_edit_ {};
        components::ScriptEditor* script_edit_ {};
        QPushButton*              commit_button_ {};
        QPushButton*              verify_button_ {};
        widgets::StatusLabel*     status_ {};
    };

} // namespace slopkit::ui::dialogs
