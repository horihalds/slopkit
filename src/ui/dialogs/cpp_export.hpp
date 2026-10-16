#pragma once

#include <QDialog>
#include <QString>

class QPlainTextEdit;

namespace slopkit::ui::widgets
{
    class StatusLabel;
}

namespace slopkit::ui::dialogs
{
    // The generated C++ header, shown before it is written. The text comes in
    // from the caller, so the dialog reads no project and touches no target.
    class CppExportDialog : public QDialog
    {
        Q_OBJECT

    public:
        explicit CppExportDialog(QString text, QWidget* parent = nullptr);

        void set_dialog_directory(const QString& directory);

        // The directory the save prompt starts in.
        [[nodiscard]] QString dialog_directory() const;

        // The write without its prompt, so a test can drive the save path.
        void save_to(const QString& path);

    signals:
        void headerSaved(const QString& path);

    private:
        void copy();
        void save_as();

        QPlainTextEdit*       view_ {};
        widgets::StatusLabel* status_ {};
        QString               dialog_directory_;
    };

}
