#pragma once

#include <cstddef>

#include <QDialog>
#include <QString>

namespace slopkit::ui::dialogs
{

    // What the user wants to do when opening a table would replace the one that
    // is already open.
    enum class TableConflictChoice
    {
        cancel,
        overwrite,
        merge,
    };

    // The facts the prompt reports: both files and both entry counts.
    struct TableConflictInfo
    {
        QString     incoming_path;
        std::size_t incoming_entries {};
        QString     current_path; // empty when the open table was never saved
        std::size_t current_entries {};
    };

    // The themed Cancel/Overwrite/Merge prompt shown before an open table is
    // replaced. Merge is the default action; Esc and the window close button map
    // to Cancel.
    class TableConflictDialog : public QDialog
    {
        Q_OBJECT

    public:
        explicit TableConflictDialog(const TableConflictInfo& info, QWidget* parent = nullptr);

        // Runs the dialog modally; the platform's decoration gets the title on
        // its own, without the application display name appended.
        int exec() override;

        [[nodiscard]] TableConflictChoice choice() const noexcept;

    protected:
        void showEvent(QShowEvent* event) override;

    private:
        TableConflictChoice choice_ {TableConflictChoice::cancel};
        QWidget*            default_widget_ {};
    };

    // Runs the prompt modally and returns the chosen action.
    [[nodiscard]] TableConflictChoice ask_table_conflict(QWidget* parent, const TableConflictInfo& info);

} // namespace slopkit::ui::dialogs
