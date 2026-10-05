#pragma once

#include <functional>
#include <optional>

#include <QDialog>
#include <QString>

class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;

namespace slopkit::ui::widgets
{
    class StatusLabel;

    // Returns an error message, or an empty string when `text` is acceptable.
    using InputValidator = std::function<QString(const QString&)>;

    // Everything an InputBox needs. An empty `ok_text` falls back to "OK"; an
    // empty `validate` leaves the field unvalidated.
    struct InputBoxOptions
    {
        QString        title;
        QString        label;
        QString        initial;
        QString        placeholder;
        QString        ok_text;
        bool           monospace {false};
        InputValidator validate;
    };

    // A themed single-line input dialog. An optional validator runs on every
    // edit and again on accept; while the text is invalid the OK button stays
    // disabled and the validator's message is shown beneath the field. Nothing
    // about its position is hard-coded - the compositor places it.
    class InputBox : public QDialog
    {
        Q_OBJECT

    public:
        explicit InputBox(InputBoxOptions options, QWidget* parent = nullptr);

        // Runs the box modally. The platform's window-title decoration is given
        // the title on its own; the application display name is not appended.
        int exec() override;

        [[nodiscard]] QString      text() const;
        [[nodiscard]] QLineEdit*   line_edit() const noexcept;
        [[nodiscard]] QPushButton* accept_button() const noexcept;

    public slots:
        // Re-runs the validator and refuses to close while the text is invalid.
        void accept() override;

    protected:
        void showEvent(QShowEvent* event) override;

    private:
        void validate();
        void update_minimum_width();

        InputBoxOptions options_;
        QLabel*         label_ {};
        QLineEdit*      edit_ {};
        StatusLabel*    status_ {};
        QPushButton*    accept_button_ {};
    };

    // Shows the box modally and returns the accepted text, or nothing when it
    // was cancelled.
    [[nodiscard]] std::optional<QString> get_text(const InputBoxOptions& options, QWidget* parent);
} // namespace slopkit::ui::widgets
