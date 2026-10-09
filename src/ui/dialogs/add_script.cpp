#include "ui/dialogs/add_script.hpp"

#include <cstddef>
#include <format>
#include <string>
#include <utility>

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"
#include "ui/text.hpp"

namespace slopkit::ui::dialogs
{

    AddScriptDialog::AddScriptDialog(table::AddressTable& table, QWidget* parent) : QDialog(parent), table_(table)
    {
        setWindowTitle(tr("Add Script"));
        setMinimumWidth(520);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        auto* form = new QFormLayout();
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

        description_edit_ = new QLineEdit(this);
        description_edit_->setObjectName(QStringLiteral("description_edit"));
        form->addRow(tr("Description"), description_edit_);
        layout->addLayout(form);

        script_edit_ = new QPlainTextEdit(this);
        script_edit_->setObjectName(QStringLiteral("script_edit"));
        script_edit_->setFont(mono_font());
        script_edit_->setPlaceholderText(QStringLiteral("print(mem.read(0x4000, \"u32\"))"));
        script_edit_->setMinimumHeight(240);
        layout->addWidget(script_edit_, 1);

        status_ = new widgets::StatusLabel(this);
        layout->addWidget(status_);

        auto* buttons  = new QHBoxLayout();
        commit_button_ = new widgets::PrimaryButton(tr("Add"), this);
        commit_button_->setObjectName(QStringLiteral("commit_button"));
        auto* close_button = widgets::secondary_button(tr("Close"), this);
        buttons->addWidget(commit_button_);
        buttons->addWidget(close_button);
        buttons->addStretch(1);
        layout->addLayout(buttons);

        connect(commit_button_, &QPushButton::clicked, this, &AddScriptDialog::commit);
        connect(close_button, &QPushButton::clicked, this, &QDialog::close);

        reset_for_add();
    }

    void AddScriptDialog::reset_for_add()
    {
        editing_.reset();
        setWindowTitle(tr("Add Script"));
        commit_button_->setText(tr("Add"));
        description_edit_->setText(tr("New script"));
        script_edit_->clear();
        status_->clear_status();
    }

    void AddScriptDialog::edit_entry(std::size_t row)
    {
        if (!table_.valid_index(row) || table_.entries()[row].kind != table::EntryKind::script)
        {
            reset_for_add();
            return;
        }

        editing_ = row;
        setWindowTitle(tr("Edit Script"));
        commit_button_->setText(tr("Save"));
        description_edit_->setText(to_qstring(table_.entries()[row].description));
        script_edit_->setPlainText(to_qstring(table_.entries()[row].script));
        status_->clear_status();
    }

    void AddScriptDialog::commit()
    {
        // An empty description would still sanitise to a member name, but a row
        // the user cannot tell apart is refused instead.
        if (description_edit_->text().trimmed().isEmpty())
        {
            log::warning(log::category::ui, "script rejected: empty description");
            status_->set_status(widgets::StatusKind::error, tr("Description must not be empty."));
            return;
        }

        const std::string description = description_edit_->text().toStdString();
        const std::string script      = script_edit_->toPlainText().toStdString();

        if (editing_.has_value() && table_.set_description(*editing_, description)
            && table_.set_script(*editing_, script))
        {
            log::info(log::category::ui,
                      std::format("script edited at index {} ({} byte(s))", *editing_, script.size()));
        }
        else
        {
            const std::size_t added = table_.add_script(description, script);
            log::info(log::category::ui, std::format("script added at index {} ({} byte(s))", added, script.size()));
        }

        // A confirmed add or edit closes the dialog; the row is the feedback.
        accept();
    }

} // namespace slopkit::ui::dialogs
