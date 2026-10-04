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

    // The presentation kind of a MessageBox; it picks the themed glyph and the
    // status colour of the main text.
    enum class MessageBoxIcon
    {
        none,
        information,
        question,
        warning,
        error,
        success,
    };

    // A standard button a MessageBox can offer. The values are powers of two so
    // the set is a proper QFlags bitmask.
    enum class MessageBoxButton
    {
        ok     = 0x1,
        cancel = 0x2,
        yes    = 0x4,
        no     = 0x8,
        close  = 0x10,
    };
    Q_DECLARE_FLAGS(MessageBoxButtons, MessageBoxButton)

    // The button the user activated; `none` means the box was dismissed through
    // Esc, the window close button or reject().
    enum class MessageBoxResult
    {
        none,
        ok,
        cancel,
        yes,
        no,
        close,
    };

    // A themed, modal message box: an optional kind glyph beside a word-wrapped
    // main text and an optional muted informative line, over a right-aligned row
    // of standard buttons. Nothing about its position is hard-coded; the
    // compositor places it and the layouts size it (the window is only kept at
    // least as wide as its title).
    class MessageBox : public QDialog
    {
        Q_OBJECT

    public:
        explicit MessageBox(QWidget* parent = nullptr);

        // Runs the box modally. The platform's window-title decoration is given
        // the title on its own; the application display name is not appended.
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

    // Runs a configurable message box and returns its result.
    [[nodiscard]] MessageBoxResult message_box(QWidget*          parent,
                                               MessageBoxIcon    icon,
                                               const QString&    title,
                                               const QString&    text,
                                               MessageBoxButtons buttons,
                                               MessageBoxButton  default_button = MessageBoxButton::ok);

    // Yes/No confirmation; only Yes confirms, dismissal reports false.
    [[nodiscard]] bool confirm(QWidget*         parent,
                               const QString&   title,
                               const QString&   text,
                               MessageBoxButton default_button = MessageBoxButton::no);

    // OK/Cancel confirmation; only OK confirms, dismissal reports false.
    [[nodiscard]] bool ok_cancel(QWidget*         parent,
                                 const QString&   title,
                                 const QString&   text,
                                 MessageBoxButton default_button = MessageBoxButton::ok);

    // Single-OK notices.
    void information(QWidget* parent, const QString& title, const QString& text);
    void warning(QWidget* parent, const QString& title, const QString& text);
    void error(QWidget* parent, const QString& title, const QString& text);

} // namespace slopkit::ui::widgets

Q_DECLARE_OPERATORS_FOR_FLAGS(slopkit::ui::widgets::MessageBoxButtons)
