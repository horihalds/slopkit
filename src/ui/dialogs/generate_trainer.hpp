#pragma once

#include <cstddef>

#include <QDialog>
#include <QString>

#include "trainer/generate.hpp"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace slopkit::ui::widgets
{
    class StatusLabel;
}

namespace slopkit::ui::dialogs
{
    // Turns the open table into standalone trainers. Generation only copies a
    // runtime template and appends the payload, so the dialog reads no target and
    // runs on the UI thread.
    class GenerateTrainerDialog : public QDialog
    {
        Q_OBJECT

    public:
        explicit GenerateTrainerDialog(trainer::Request request, QWidget* parent = nullptr);

        [[nodiscard]] QString output_directory() const;
        void                  set_output_directory(const QString& directory);

        [[nodiscard]] QString base_name() const;

        [[nodiscard]] bool linux_target() const;
        [[nodiscard]] bool windows_target() const;
        void               set_linux_target(bool enabled);
        void               set_windows_target(bool enabled);

        // What will be embedded: `N value entries, M script entries skipped`.
        [[nodiscard]] QString summary_text() const;

        // The Generate button's state: at least one target, a directory and a name.
        [[nodiscard]] bool can_generate() const;

        // The write without its prompt, so a test can drive generation.
        void generate();

        // The paths that were written, or the reason nothing was.
        [[nodiscard]] QString result_text() const;

    signals:
        void trainersGenerated(const QString& directory);

    private:
        void browse();
        void update_state();

        trainer::Request      request_;
        std::size_t           value_entries_ {};
        std::size_t           skipped_scripts_ {};
        QCheckBox*            linux_box_ {};
        QCheckBox*            windows_box_ {};
        QLineEdit*            directory_edit_ {};
        QLineEdit*            name_edit_ {};
        QLabel*               summary_ {};
        widgets::StatusLabel* status_ {};
        QPlainTextEdit*       results_ {};
        QPushButton*          generate_button_ {};
    };

}
