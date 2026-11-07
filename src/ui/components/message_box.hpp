#pragma once

#include <QDialog>
#include <QString>

class QHBoxLayout;
class QLabel;
class QPushButton;
class QShowEvent;

namespace slopkit::ui::widgets
{
    class StatusLabel;

    enum class MessageBoxIcon
    {
        none,
        information,
        question,
        warning,
        error,
        success,
    };

    enum class MessageBoxButton
    {
        ok     = 0x1,
        cancel = 0x2,
        yes    = 0x4,
        no     = 0x8,
        close  = 0x10,
    };
    Q_DECLARE_FLAGS(MessageBoxButtons, MessageBoxButton)

    enum class MessageBoxResult
    {
        none,
        ok,
        cancel,
        yes,
        no,
        close,
    };

    class MessageBox : public QDialog
    {
        Q_OBJECT

    public:
        explicit MessageBox(QWidget* parent = nullptr);

        int exec() override;

        void set_icon(MessageBoxIcon icon);
        void set_buttons(MessageBoxButtons buttons);
        void set_title(const QString& title);
        void set_text(const QString& text);
        void set_informative_text(const QString& text);
        void set_default_button(MessageBoxButton which);

        [[nodiscard]] MessageBoxIcon    icon() const noexcept;
        [[nodiscard]] MessageBoxButtons buttons() const noexcept;
        [[nodiscard]] MessageBoxButton  default_button() const noexcept;
        [[nodiscard]] QString           text() const;
        [[nodiscard]] QString           informative_text() const;
        [[nodiscard]] MessageBoxResult  result() const noexcept;

    protected:
        void showEvent(QShowEvent* event) override;

    private:
        void rebuild_buttons();
        void update_icon();
        void update_minimum_width();

        QLabel*      icon_label_ {};
        StatusLabel* text_label_ {};
        QLabel*      informative_label_ {};
        QHBoxLayout* button_row_ {};
        QPushButton* default_widget_ {};

        MessageBoxIcon    icon_ {MessageBoxIcon::none};
        MessageBoxButtons buttons_ {MessageBoxButton::ok};
        MessageBoxButton  default_button_ {MessageBoxButton::ok};
        MessageBoxResult  result_ {MessageBoxResult::none};
    };

    [[nodiscard]] MessageBoxResult message_box(QWidget*          parent,
                                               MessageBoxIcon    icon,
                                               const QString&    title,
                                               const QString&    text,
                                               MessageBoxButtons buttons,
                                               MessageBoxButton  default_button = MessageBoxButton::ok);

    [[nodiscard]] bool confirm(QWidget*         parent,
                               const QString&   title,
                               const QString&   text,
                               MessageBoxButton default_button = MessageBoxButton::no);

    [[nodiscard]] bool ok_cancel(QWidget*         parent,
                                 const QString&   title,
                                 const QString&   text,
                                 MessageBoxButton default_button = MessageBoxButton::ok);

    void information(QWidget* parent, const QString& title, const QString& text);
    void warning(QWidget* parent, const QString& title, const QString& text);
    void error(QWidget* parent, const QString& title, const QString& text);

}

Q_DECLARE_OPERATORS_FOR_FLAGS(slopkit::ui::widgets::MessageBoxButtons)
