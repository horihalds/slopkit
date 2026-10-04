#include "ui/components/message_box.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QStyle>
#include <QVBoxLayout>

#include "ui/components/widgets.hpp"

namespace slopkit::ui::widgets
{
    namespace
    {
        // Canonical left-to-right order the standard buttons are laid out in.
        constexpr std::array kButtonOrder {
            MessageBoxButton::yes,
            MessageBoxButton::no,
            MessageBoxButton::ok,
            MessageBoxButton::cancel,
            MessageBoxButton::close,
        };

        constexpr ActionIcon action_icon_for(MessageBoxIcon icon)
        {
            switch (icon)
            {
            case MessageBoxIcon::none:
            case MessageBoxIcon::information:
            case MessageBoxIcon::question:
                return ActionIcon::info;
            case MessageBoxIcon::warning:
                return ActionIcon::warning;
            case MessageBoxIcon::error:
                return ActionIcon::error;
            case MessageBoxIcon::success:
                return ActionIcon::ok;
            }
            std::unreachable();
        }

        constexpr StatusKind status_for(MessageBoxIcon icon)
        {
            switch (icon)
            {
            case MessageBoxIcon::none:
            case MessageBoxIcon::information:
            case MessageBoxIcon::question:
                return StatusKind::info;
            case MessageBoxIcon::warning:
                return StatusKind::warning;
            case MessageBoxIcon::error:
                return StatusKind::error;
            case MessageBoxIcon::success:
                return StatusKind::success;
            }
            std::unreachable();
        }

        constexpr MessageBoxResult result_for(MessageBoxButton button)
        {
            switch (button)
            {
            case MessageBoxButton::ok:
                return MessageBoxResult::ok;
            case MessageBoxButton::cancel:
                return MessageBoxResult::cancel;
            case MessageBoxButton::yes:
                return MessageBoxResult::yes;
            case MessageBoxButton::no:
                return MessageBoxResult::no;
            case MessageBoxButton::close:
                return MessageBoxResult::close;
            }
            std::unreachable();
        }

        QString button_label(MessageBoxButton button)
        {
            switch (button)
            {
            case MessageBoxButton::ok:
                return MessageBox::tr("OK");
            case MessageBoxButton::cancel:
                return MessageBox::tr("Cancel");
            case MessageBoxButton::yes:
                return MessageBox::tr("Yes");
            case MessageBoxButton::no:
                return MessageBox::tr("No");
            case MessageBoxButton::close:
                return MessageBox::tr("Close");
            }
            std::unreachable();
        }
    } // namespace

    MessageBox::MessageBox(QWidget* parent) : QDialog(parent)
    {
        // Modal to the owner window so it keeps input focus; the compositor
        // alone decides where the box is placed.
        setWindowModality(Qt::WindowModal);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(10);

        auto* content = new QHBoxLayout();
        content->setSpacing(12);

        icon_label_ = new QLabel(this);
        icon_label_->setAlignment(Qt::AlignTop);
        content->addWidget(icon_label_, 0, Qt::AlignTop);

        auto* text_column = new QVBoxLayout();
        text_column->setSpacing(6);
        text_label_ = new StatusLabel(this);
        text_column->addWidget(text_label_);
        informative_label_ = hint_text(QString(), this);
        informative_label_->setVisible(false);
        text_column->addWidget(informative_label_);
        text_column->addStretch(1);
        content->addLayout(text_column, 1);

        layout->addLayout(content, 1);

        button_row_ = new QHBoxLayout();
        button_row_->setSpacing(6);
        layout->addLayout(button_row_);

        rebuild_buttons();
        update_icon();
    }

    int MessageBox::exec()
    {
        // Qt's platform plugins append the application display name to a window
        // whose title does not already end with it, which would turn the box's
        // title into "Delete Entry - slopkit". The box shows its own title only,
        // so hide the display name while the modal loop runs. Title formatting
        // happens when a window is created, so no other window is affected.
        const QString display_name = QGuiApplication::applicationDisplayName();
        QGuiApplication::setApplicationDisplayName(QString());
        const int code = QDialog::exec();
        QGuiApplication::setApplicationDisplayName(display_name);
        return code;
    }

    void MessageBox::set_icon(MessageBoxIcon icon)
    {
        icon_ = icon;
        update_icon();
        // The main text follows the kind's status colour.
        text_label_->set_status(status_for(icon_), text_label_->text());
        update_minimum_width();
    }

    void MessageBox::set_buttons(MessageBoxButtons buttons)
    {
        buttons_ = buttons;
        rebuild_buttons();
    }

    void MessageBox::set_title(const QString& title)
    {
        setWindowTitle(title);
        update_minimum_width();
    }

    void MessageBox::set_text(const QString& text)
    {
        text_label_->set_status(status_for(icon_), text);
        update_minimum_width();
    }

    void MessageBox::set_informative_text(const QString& text)
    {
        informative_label_->setText(text);
        informative_label_->setVisible(!text.isEmpty());
        update_minimum_width();
    }

    void MessageBox::set_default_button(MessageBoxButton which)
    {
        default_button_ = which;
        rebuild_buttons();
    }

    MessageBoxIcon MessageBox::icon() const noexcept
    {
        return icon_;
    }

    MessageBoxButtons MessageBox::buttons() const noexcept
    {
        return buttons_;
    }

    MessageBoxButton MessageBox::default_button() const noexcept
    {
        return default_button_;
    }

    QString MessageBox::text() const
    {
        return text_label_->text();
    }

    QString MessageBox::informative_text() const
    {
        return informative_label_->text();
    }

    MessageBoxResult MessageBox::result() const noexcept
    {
        return result_;
    }

    void MessageBox::showEvent(QShowEvent* event)
    {
        QDialog::showEvent(event);
        if (default_widget_ != nullptr)
        {
            default_widget_->setFocus(Qt::OtherFocusReason);
        }
    }

    void MessageBox::rebuild_buttons()
    {
        default_widget_ = nullptr;
        while (QLayoutItem* item = button_row_->takeAt(0))
        {
            delete item->widget();
            delete item;
        }

        // The leading stretch keeps the button row right-aligned.
        button_row_->addStretch(1);
        for (const MessageBoxButton which : kButtonOrder)
        {
            if (!buttons_.testFlag(which))
            {
                continue;
            }

            const bool   is_default = which == default_button_;
            QPushButton* button     = nullptr;
            if (is_default)
            {
                button = new PrimaryButton(button_label(which), this);
            }
            else
            {
                button = secondary_button(button_label(which), this);
            }
            button->setAutoDefault(true);
            button->setDefault(is_default);
            connect(button,
                    &QPushButton::clicked,
                    this,
                    [this, which]
                    {
                        result_ = result_for(which);
                        accept();
                    });
            button_row_->addWidget(button);
            if (is_default)
            {
                default_widget_ = button;
            }
        }
        update_minimum_width();
    }

    void MessageBox::update_minimum_width()
    {
        // Like QMessageBox, keep the box at least as wide as its natural layout
        // so it never opens as a sliver; an explicit minimum smaller than the
        // natural width is what a window stack may otherwise size the box to.
        // The window title can be wider still, so cover that too. Both values
        // are logical and come from the layouts and font metrics, so Qt handles
        // scaling.
        int minimum = sizeHint().width();
        if (!windowTitle().isEmpty())
        {
            QFont title_font = font();
            title_font.setBold(true);
            minimum = std::max(minimum, QFontMetrics(title_font).horizontalAdvance(windowTitle()));
        }
        setMinimumWidth(minimum);
    }

    void MessageBox::update_icon()
    {
        if (icon_ == MessageBoxIcon::none)
        {
            icon_label_->clear();
            icon_label_->setVisible(false);
            return;
        }

        const int extent = style()->pixelMetric(QStyle::PM_MessageBoxIconSize);
        icon_label_->setPixmap(action_icon(action_icon_for(icon_)).pixmap(extent, extent));
        icon_label_->setVisible(true);
    }

    MessageBoxResult message_box(QWidget*          parent,
                                 MessageBoxIcon    icon,
                                 const QString&    title,
                                 const QString&    text,
                                 MessageBoxButtons buttons,
                                 MessageBoxButton  default_button)
    {
        MessageBox box(parent);
        box.set_icon(icon);
        box.set_title(title);
        box.set_text(text);
        box.set_buttons(buttons);
        box.set_default_button(default_button);
        box.exec();
        return box.result();
    }

    bool confirm(QWidget* parent, const QString& title, const QString& text, MessageBoxButton default_button)
    {
        return message_box(parent,
                           MessageBoxIcon::question,
                           title,
                           text,
                           MessageBoxButton::yes | MessageBoxButton::no,
                           default_button)
            == MessageBoxResult::yes;
    }

    bool ok_cancel(QWidget* parent, const QString& title, const QString& text, MessageBoxButton default_button)
    {
        return message_box(parent,
                           MessageBoxIcon::question,
                           title,
                           text,
                           MessageBoxButton::ok | MessageBoxButton::cancel,
                           default_button)
            == MessageBoxResult::ok;
    }

    void information(QWidget* parent, const QString& title, const QString& text)
    {
        (void)message_box(parent, MessageBoxIcon::information, title, text, MessageBoxButton::ok);
    }

    void warning(QWidget* parent, const QString& title, const QString& text)
    {
        (void)message_box(parent, MessageBoxIcon::warning, title, text, MessageBoxButton::ok);
    }

    void error(QWidget* parent, const QString& title, const QString& text)
    {
        (void)message_box(parent, MessageBoxIcon::error, title, text, MessageBoxButton::ok);
    }

} // namespace slopkit::ui::widgets
