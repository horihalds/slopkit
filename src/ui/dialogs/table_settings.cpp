#include "ui/dialogs/table_settings.hpp"

#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
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

    TableSettingsDialog::TableSettingsDialog(table::AddressTable& table, QWidget* parent)
        : QDialog(parent), table_(table)
    {
        setWindowTitle(tr("Table Settings"));
        setMinimumWidth(360);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        layout->addWidget(widgets::section_header(tr("Target process"), this));

        auto* form = new QFormLayout();
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

        target_edit_ = new QLineEdit(this);
        target_edit_->setFont(mono_font());
        target_edit_->setPlaceholderText(QStringLiteral("game"));
        form->addRow(tr("Name"), target_edit_);
        layout->addLayout(form);

        auto_attach_check_    = new QCheckBox(tr("Auto attach on open"), this);
        match_exe_path_check_ = new QCheckBox(tr("Match by executable path"), this);
        layout->addWidget(auto_attach_check_);
        layout->addWidget(match_exe_path_check_);

        status_ = new widgets::StatusLabel(this);
        layout->addWidget(status_);

        auto* buttons      = new QHBoxLayout();
        auto* close_button = widgets::secondary_button(tr("Close"), this);
        buttons->addStretch(1);
        buttons->addWidget(close_button);
        layout->addLayout(buttons);

        connect(target_edit_, &QLineEdit::editingFinished, this, &TableSettingsDialog::commit);
        connect(auto_attach_check_, &QCheckBox::toggled, this, &TableSettingsDialog::commit_auto_attach);
        connect(match_exe_path_check_, &QCheckBox::toggled, this, &TableSettingsDialog::commit);
        connect(close_button, &QPushButton::clicked, this, &QDialog::close);
    }

    void TableSettingsDialog::showEvent(QShowEvent* event)
    {
        QDialog::showEvent(event);

        const table::TableSettings& settings = table_.settings();
        updating_                            = true;
        target_edit_->setText(QString::fromStdString(settings.target_process));
        auto_attach_check_->setChecked(settings.auto_attach);
        match_exe_path_check_->setChecked(settings.match_exe_path);
        updating_ = false;

        status_->clear_status();
    }

    void TableSettingsDialog::commit_auto_attach()
    {
        if (updating_)
        {
            return;
        }
        if (auto_attach_check_->isChecked() && target_edit_->text().trimmed().isEmpty())
        {
            log::warning(log::category::ui, "auto attach refused: empty target process");
            status_->set_status(widgets::StatusKind::error,
                                tr("Set a target process name before enabling auto attach."));
            const QSignalBlocker blocker(auto_attach_check_);
            auto_attach_check_->setChecked(false);
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
        settings.auto_attach           = auto_attach_check_->isChecked();
        settings.match_exe_path        = match_exe_path_check_->isChecked();
        status_->set_status(widgets::StatusKind::info, tr("Table settings updated."));
    }

} // namespace slopkit::ui::dialogs
