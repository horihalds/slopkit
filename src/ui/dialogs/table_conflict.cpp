#include "ui/dialogs/table_conflict.hpp"

#include <utility>

#include <QDir>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QStyle>
#include <QVBoxLayout>

#include "ui/components/widgets.hpp"

namespace slopkit::ui::dialogs
{
    namespace
    {
        QString display_path(const QString& path)
        {
            return path.isEmpty() ? TableConflictDialog::tr("(never saved)") : QDir::toNativeSeparators(path);
        }
    } // namespace

    TableConflictDialog::TableConflictDialog(const TableConflictInfo& info, QWidget* parent) : QDialog(parent)
    {
        setWindowModality(Qt::WindowModal);
        setWindowTitle(tr("Open Table"));

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(10);

        auto* content = new QHBoxLayout();
        content->setSpacing(12);

        auto*     icon_label = new QLabel(this);
        const int extent     = style()->pixelMetric(QStyle::PM_MessageBoxIconSize);
        icon_label->setPixmap(widgets::action_icon(widgets::ActionIcon::warning).pixmap(extent, extent));
        icon_label->setAlignment(Qt::AlignTop);
        content->addWidget(icon_label, 0, Qt::AlignTop);

        auto* text_column = new QVBoxLayout();
        text_column->setSpacing(6);

        auto* main_label = new widgets::StatusLabel(this);
        main_label->set_status(widgets::StatusKind::warning,
                               tr("Open table: %1 (%2 entries)\nFile: %3 (%4 entries)")
                                   .arg(display_path(info.current_path))
                                   .arg(QString::number(info.current_entries))
                                   .arg(display_path(info.incoming_path))
                                   .arg(QString::number(info.incoming_entries)));
        text_column->addWidget(main_label);

        auto* hint_label = widgets::hint_text(tr("Cancel keeps the open table, Overwrite replaces it with the file, "
                                                 "and Merge appends the file's entries that are not already present."),
                                              this);
        text_column->addWidget(hint_label);
        text_column->addStretch(1);
        content->addLayout(text_column, 1);

        layout->addLayout(content, 1);

        auto* button_row = new QHBoxLayout();
        button_row->setSpacing(6);
        button_row->addStretch(1);

        auto add_choice = [&](TableConflictChoice choice, QPushButton* button)
        {
            button->setAutoDefault(true);
            connect(button,
                    &QPushButton::clicked,
                    this,
                    [this, choice]
                    {
                        choice_ = choice;
                        accept();
                    });
            button_row->addWidget(button);
        };

        add_choice(TableConflictChoice::cancel, widgets::secondary_button(tr("Cancel"), this));
        add_choice(TableConflictChoice::overwrite, widgets::secondary_button(tr("Overwrite"), this));
        auto* merge_button = new widgets::PrimaryButton(tr("Merge"), this);
        merge_button->setDefault(true);
        default_widget_ = merge_button;
        add_choice(TableConflictChoice::merge, merge_button);

        layout->addLayout(button_row);
    }

    int TableConflictDialog::exec()
    {
        // Qt appends the application display name to a window whose title does
        // not already end with it; the prompt shows its own title only.
        const QString display_name = QGuiApplication::applicationDisplayName();
        QGuiApplication::setApplicationDisplayName(QString());
        const int code = QDialog::exec();
        QGuiApplication::setApplicationDisplayName(display_name);
        return code;
    }

    TableConflictChoice TableConflictDialog::choice() const noexcept
    {
        return choice_;
    }

    void TableConflictDialog::showEvent(QShowEvent* event)
    {
        QDialog::showEvent(event);
        if (default_widget_ != nullptr)
        {
            default_widget_->setFocus(Qt::OtherFocusReason);
        }
    }

    TableConflictChoice ask_table_conflict(QWidget* parent, const TableConflictInfo& info)
    {
        TableConflictDialog dialog(info, parent);
        dialog.exec();
        return dialog.choice();
    }

} // namespace slopkit::ui::dialogs
