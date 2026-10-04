#include "ui/dialogs/table_settings.hpp"

#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"

namespace slopkit::ui::dialogs
{

    TableSettingsDialog::TableSettingsDialog(table::AddressTable&           table,
                                             const process::AttachedTarget& target,
                                             QWidget*                       parent)
        : QDialog(parent), table_(table), target_(target)
    {
        setWindowTitle(tr("Table Settings"));
        setMinimumWidth(460);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(10);

        auto* target_panel = new widgets::Panel(tr("Target process"), this);
        auto* target_body  = target_panel->body();

        auto* form = new QFormLayout();
        form->setContentsMargins(0, 0, 0, 0);
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

        target_edit_ = new QLineEdit(this);
        target_edit_->setObjectName(QStringLiteral("target_edit"));
        target_edit_->setFont(mono_font());
        target_edit_->setPlaceholderText(QStringLiteral("game"));
        form->addRow(tr("Name"), target_edit_);

        exe_path_edit_ = new QLineEdit(this);
        exe_path_edit_->setObjectName(QStringLiteral("exe_path_edit"));
        exe_path_edit_->setFont(mono_font());
        exe_path_edit_->setPlaceholderText(QStringLiteral("/usr/bin/game"));
        form->addRow(tr("Executable path"), exe_path_edit_);
        target_body->addLayout(form);

        auto_attach_check_    = new QCheckBox(tr("Auto attach on open"), this);
        match_exe_path_check_ = new QCheckBox(tr("Match by executable path"), this);
        target_body->addWidget(auto_attach_check_);
        target_body->addWidget(match_exe_path_check_);
        target_body->addWidget(widgets::hint_text(
            tr("The ticked checkbox decides whether the name or the executable path identifies the process."), this));

        layout->addWidget(target_panel);

        auto* attached_panel = new widgets::Panel(tr("Attached process"), this);
        auto* attached_body  = attached_panel->body();

        attached_hint_ = widgets::hint_text(tr("No process is attached."), this);
        attached_body->addWidget(attached_hint_);

        attached_name_ = new QLabel(this);
        attached_name_->setWordWrap(true);
        attached_body->addWidget(attached_name_);

        attached_plugin_ = new QLabel(this);
        attached_plugin_->setWordWrap(true);
        attached_body->addWidget(attached_plugin_);

        attached_exe_path_ = new QLabel(this);
        attached_exe_path_->setFont(mono_font());
        attached_exe_path_->setWordWrap(true);
        attached_body->addWidget(attached_exe_path_);

        use_attached_button_ = widgets::secondary_button(tr("Use Attached Process"), this);
        use_attached_button_->setObjectName(QStringLiteral("use_attached_button"));
        use_attached_button_->setIcon(widgets::action_icon(widgets::ActionIcon::target));
        attached_body->addWidget(use_attached_button_);

        layout->addWidget(attached_panel);

        status_ = new widgets::StatusLabel(this);
        layout->addWidget(status_);

        auto* buttons      = new QHBoxLayout();
        auto* close_button = widgets::secondary_button(tr("Close"), this);
        buttons->addStretch(1);
        buttons->addWidget(close_button);
        layout->addLayout(buttons);

        connect(target_edit_, &QLineEdit::editingFinished, this, &TableSettingsDialog::commit);
        connect(exe_path_edit_, &QLineEdit::editingFinished, this, &TableSettingsDialog::commit);
        connect(auto_attach_check_, &QCheckBox::toggled, this, &TableSettingsDialog::commit_auto_attach);
        connect(match_exe_path_check_, &QCheckBox::toggled, this, &TableSettingsDialog::commit_match_exe_path);
        connect(use_attached_button_, &QPushButton::clicked, this, &TableSettingsDialog::fill_from_target);
        connect(close_button, &QPushButton::clicked, this, &QDialog::close);

        refresh_target();
    }

    void TableSettingsDialog::showEvent(QShowEvent* event)
    {
        QDialog::showEvent(event);

        const table::TableSettings& settings = table_.settings();
        updating_                            = true;
        target_edit_->setText(QString::fromStdString(settings.target_process));
        exe_path_edit_->setText(QString::fromStdString(settings.exe_path));
        auto_attach_check_->setChecked(settings.auto_attach);
        match_exe_path_check_->setChecked(settings.match_exe_path);
        updating_ = false;

        status_->clear_status();
        refresh_target();
    }

    void TableSettingsDialog::refresh_target()
    {
        if (rendered_ && rendered_pid_ == target_.pid && rendered_name_ == target_.name
            && rendered_exe_path_ == target_.exe_path && rendered_live_ == target_.session_live)
        {
            return;
        }
        rendered_          = true;
        rendered_pid_      = target_.pid;
        rendered_name_     = target_.name;
        rendered_exe_path_ = target_.exe_path;
        rendered_live_     = target_.session_live;

        const bool attached = target_.valid();
        use_attached_button_->setEnabled(attached);

        attached_hint_->setVisible(!attached);
        attached_name_->setVisible(attached);
        attached_plugin_->setVisible(attached);
        attached_exe_path_->setVisible(attached);

        if (!attached)
        {
            attached_hint_->setText(tr("No process is attached."));
            attached_name_->clear();
            attached_plugin_->clear();
            attached_exe_path_->clear();
            return;
        }

        const QString display = target_.name.empty() ? tr("process") : QString::fromStdString(target_.name);
        attached_name_->setText(tr("%1 (%2)").arg(display).arg(target_.pid));
        attached_plugin_->setText(target_.plugin_id.empty()
                                      ? tr("Plugin: unknown")
                                      : tr("Plugin: %1").arg(QString::fromStdString(target_.plugin_id)));
        attached_exe_path_->setText(target_.exe_path.empty() ? tr("executable path unknown")
                                                             : QString::fromStdString(target_.exe_path));
    }

    void TableSettingsDialog::fill_from_target()
    {
        if (!target_.valid())
        {
            return;
        }

        target_edit_->setText(QString::fromStdString(target_.name));
        if (!target_.exe_path.empty())
        {
            exe_path_edit_->setText(QString::fromStdString(target_.exe_path));
        }
        commit();

        // The button copies the identity only; the user ticks the checkboxes.
        if (target_.exe_path.empty())
        {
            status_->set_status(widgets::StatusKind::warning,
                                tr("The attached process has no known executable path; only the name was filled."));
        }
    }

    void TableSettingsDialog::commit_auto_attach()
    {
        if (updating_)
        {
            return;
        }
        const bool    by_path  = match_exe_path_check_->isChecked();
        const QString selected = by_path ? exe_path_edit_->text() : target_edit_->text();
        if (auto_attach_check_->isChecked() && selected.trimmed().isEmpty())
        {
            log::warning(log::category::ui, "auto attach refused: empty target identifier");
            status_->set_status(widgets::StatusKind::error,
                                tr("Set the %1 before enabling auto attach.")
                                    .arg(by_path ? tr("executable path") : tr("process name")));
            const QSignalBlocker blocker(auto_attach_check_);
            auto_attach_check_->setChecked(false);
            return;
        }
        commit();
    }

    void TableSettingsDialog::commit_match_exe_path()
    {
        if (updating_)
        {
            return;
        }
        if (auto_attach_check_->isChecked() && match_exe_path_check_->isChecked()
            && exe_path_edit_->text().trimmed().isEmpty())
        {
            log::warning(log::category::ui, "match by executable path refused: empty exe path");
            status_->set_status(widgets::StatusKind::error,
                                tr("Set an executable path before matching by it with auto attach on."));
            const QSignalBlocker blocker(match_exe_path_check_);
            match_exe_path_check_->setChecked(false);
            return;
        }
        commit();
    }

    void TableSettingsDialog::commit()
    {
        if (updating_)
        {
            return;
        }
        table::TableSettings& settings = table_.settings();
        settings.target_process        = target_edit_->text().toStdString();
        settings.exe_path              = exe_path_edit_->text().toStdString();
        settings.auto_attach           = auto_attach_check_->isChecked();
        settings.match_exe_path        = match_exe_path_check_->isChecked();
        status_->set_status(widgets::StatusKind::info, tr("Table settings updated."));
    }

} // namespace slopkit::ui::dialogs
