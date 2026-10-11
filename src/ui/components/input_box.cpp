#include "ui/components/input_box.hpp"

#include <algorithm>
#include <utility>

#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>

#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"

namespace slopkit::ui::widgets
{
    InputBox::InputBox(InputBoxOptions options, QWidget* parent) : QDialog(parent), options_(std::move(options))
    {
        setWindowTitle(options_.title);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(8);

        label_ = new QLabel(options_.label, this);
        label_->setVisible(!options_.label.isEmpty());
        layout->addWidget(label_);

        edit_ = new QLineEdit(options_.initial, this);
        edit_->setPlaceholderText(options_.placeholder);
        if (options_.monospace)
        {
            edit_->setFont(mono_font());
        }
        layout->addWidget(edit_);

        status_ = new StatusLabel(this);
        layout->addWidget(status_);

        auto* button_row = new QHBoxLayout();
        button_row->setSpacing(6);
        button_row->addStretch(1);

        QPushButton* cancel = secondary_button(tr("Cancel"), this);
        cancel->setAutoDefault(false);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        button_row->addWidget(cancel);

        accept_button_ = new PrimaryButton(options_.ok_text.isEmpty() ? tr("OK") : options_.ok_text, this);
        accept_button_->setAutoDefault(true);
        accept_button_->setDefault(true);
        connect(accept_button_, &QPushButton::clicked, this, &QDialog::accept);
        button_row->addWidget(accept_button_);

        layout->addLayout(button_row);

        connect(edit_,
                &QLineEdit::textChanged,
                this,
                [this](const QString&)
                {
                    validate();
                });
        connect(edit_, &QLineEdit::returnPressed, this, &InputBox::accept);

        validate();
        update_minimum_width();
    }

    int InputBox::exec()
    {
        const QString display_name = QGuiApplication::applicationDisplayName();
        QGuiApplication::setApplicationDisplayName(QString());
        const int code = QDialog::exec();
        QGuiApplication::setApplicationDisplayName(display_name);
        return code;
    }

    QString InputBox::text() const
    {
        return edit_->text();
    }

    QLineEdit* InputBox::line_edit() const noexcept
    {
        return edit_;
    }

    QPushButton* InputBox::accept_button() const noexcept
    {
        return accept_button_;
    }

    void InputBox::accept()
    {
        validate();
        if (accept_button_ != nullptr && !accept_button_->isEnabled())
        {
            return;
        }
        QDialog::accept();
    }

    void InputBox::showEvent(QShowEvent* event)
    {
        QDialog::showEvent(event);
        edit_->setFocus(Qt::OtherFocusReason);
        edit_->selectAll();
    }

    void InputBox::validate()
    {
        if (!options_.validate)
        {
            accept_button_->setEnabled(true);
            status_->clear_status();
            return;
        }

        const QString message = options_.validate(edit_->text());
        if (message.isEmpty())
        {
            accept_button_->setEnabled(true);
            status_->clear_status();
        }
        else
        {
            accept_button_->setEnabled(false);
            status_->set_status(StatusKind::error, message);
        }
    }

    void InputBox::update_minimum_width()
    {
        int minimum = sizeHint().width();
        if (!windowTitle().isEmpty())
        {
            QFont title_font = font();
            title_font.setBold(true);
            minimum = std::max(minimum, QFontMetrics(title_font).horizontalAdvance(windowTitle()));
        }
        setMinimumWidth(minimum);
    }

    std::optional<QString> get_text(const InputBoxOptions& options, QWidget* parent)
    {
        InputBox box(options, parent);
        if (box.exec() != QDialog::Accepted)
        {
            return std::nullopt;
        }
        return box.text();
    }

}
