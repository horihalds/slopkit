#pragma once

#include <cstddef>

#include <QDialog>
#include <QString>

namespace slopkit::ui::dialogs
{
    enum class TableConflictChoice
    {
        cancel,
        overwrite,
        merge,
    };

    struct TableConflictInfo
    {
        QString     incoming_path;
        std::size_t incoming_entries {};
        QString     current_path;
        std::size_t current_entries {};
    };

    class TableConflictDialog : public QDialog
    {
        Q_OBJECT

    public:
        explicit TableConflictDialog(const TableConflictInfo& info, QWidget* parent = nullptr);

        int exec() override;

        [[nodiscard]] TableConflictChoice choice() const noexcept;

    protected:
        void showEvent(QShowEvent* event) override;

    private:
        TableConflictChoice choice_ {TableConflictChoice::cancel};
        QWidget*            default_widget_ {};
    };

    [[nodiscard]] TableConflictChoice ask_table_conflict(QWidget* parent, const TableConflictInfo& info);

}
