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

    using InputValidator = std::function<QString(const QString&)>;

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

    class InputBox : public QDialog
    {
        Q_OBJECT

    public:
        explicit InputBox(InputBoxOptions options, QWidget* parent = nullptr);

        int exec() override;

        [[nodiscard]] QString      text() const;
        [[nodiscard]] QLineEdit*   line_edit() const noexcept;
        [[nodiscard]] QPushButton* accept_button() const noexcept;

    public slots:
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

    [[nodiscard]] std::optional<QString> get_text(const InputBoxOptions& options, QWidget* parent);
}
