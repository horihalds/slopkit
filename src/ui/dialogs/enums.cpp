#include "ui/dialogs/enums.hpp"

#include <cstdint>
#include <utility>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QVariant>

#include "reclass/enum.hpp"
#include "reclass/project.hpp"
#include "ui/text.hpp"

namespace slopkit::ui::dialogs
{
    namespace
    {
        constexpr int kNameColumn    = 0;
        constexpr int kValueColumn   = 1;
        constexpr int kCommentColumn = 2;

        QString cell_text(const QTableWidget* table, int row, int column)
        {
            const QTableWidgetItem* item = table->item(row, column);
            return item == nullptr ? QString {} : item->text();
        }
    }

    EnumEditorDialog::EnumEditorDialog(reclass::Project& project, QString target_name, QWidget* parent)
        : QDialog(parent), project_(project), target_name_(std::move(target_name)), creating_(target_name_.isEmpty())
    {
        build_layout();
        load();
    }

    QLineEdit* EnumEditorDialog::name_edit() const noexcept
    {
        return name_;
    }

    QComboBox* EnumEditorDialog::size_combo() const noexcept
    {
        return size_;
    }

    QTableWidget* EnumEditorDialog::values_table() const noexcept
    {
        return values_;
    }

    void EnumEditorDialog::build_layout()
    {
        setWindowTitle(tr("Enum"));

        name_ = new QLineEdit(this);

        size_ = new QComboBox(this);
        for (const std::size_t size : reclass::kEnumSizes)
        {
            size_->addItem(tr("%1 byte(s)").arg(static_cast<qulonglong>(size)),
                           QVariant::fromValue<qulonglong>(static_cast<qulonglong>(size)));
        }

        auto* fields = new QFormLayout;
        fields->addRow(tr("Name"), name_);
        fields->addRow(tr("Size"), size_);

        values_ = new QTableWidget(0, 3, this);
        values_->setHorizontalHeaderLabels({tr("Name"), tr("Value"), tr("Comment")});
        values_->horizontalHeader()->setStretchLastSection(true);
        values_->verticalHeader()->setVisible(false);

        add_value_    = new QPushButton(tr("Add value"), this);
        remove_value_ = new QPushButton(tr("Remove value"), this);

        auto* value_buttons = new QHBoxLayout;
        value_buttons->addWidget(add_value_);
        value_buttons->addWidget(remove_value_);
        value_buttons->addStretch(1);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

        auto* layout = new QVBoxLayout(this);
        layout->addLayout(fields);
        layout->addWidget(new QLabel(tr("Values"), this));
        layout->addWidget(values_);
        layout->addLayout(value_buttons);
        layout->addWidget(buttons);

        connect(add_value_,
                &QPushButton::clicked,
                this,
                [this]
                {
                    add_value_row();
                });
        connect(remove_value_,
                &QPushButton::clicked,
                this,
                [this]
                {
                    remove_value_row();
                });
        connect(buttons,
                &QDialogButtonBox::accepted,
                this,
                [this]
                {
                    if (apply())
                    {
                        accept();
                    }
                });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    }

    void EnumEditorDialog::load()
    {
        const reclass::EnumDescription* description = nullptr;
        if (!creating_)
        {
            description = project_.find_enum(target_name_.toStdString());
            creating_   = description == nullptr;
        }

        // Four bytes is the width the reference creates an enum with.
        const std::size_t width = description == nullptr ? std::size_t {4} : description->size;
        for (int index = 0; index < size_->count(); ++index)
        {
            if (size_->itemData(index).toULongLong() == static_cast<qulonglong>(width))
            {
                size_->setCurrentIndex(index);
            }
        }

        if (description == nullptr)
        {
            name_->setText(target_name_);
            return;
        }

        name_->setText(to_qstring(description->name));
        for (const reclass::EnumValue& value : description->values)
        {
            add_value_row(to_qstring(value.name), QString::number(value.value), to_qstring(value.comment));
        }
    }

    void EnumEditorDialog::add_value_row(const QString& name, const QString& value, const QString& comment)
    {
        const int row = values_->rowCount();
        values_->insertRow(row);
        values_->setItem(row, kNameColumn, new QTableWidgetItem(name));
        values_->setItem(row, kValueColumn, new QTableWidgetItem(value));
        values_->setItem(row, kCommentColumn, new QTableWidgetItem(comment));
    }

    void EnumEditorDialog::remove_value_row()
    {
        const int row = values_->currentRow();
        if (row >= 0)
        {
            values_->removeRow(row);
        }
    }

    void EnumEditorDialog::clear_values()
    {
        values_->setRowCount(0);
    }

    bool EnumEditorDialog::collect(reclass::EnumDescription& description, QString& error) const
    {
        const QString name = name_->text().trimmed();
        if (name.isEmpty())
        {
            error = tr("An enum name is required.");
            return false;
        }

        const std::size_t size = static_cast<std::size_t>(size_->currentData().toULongLong());
        if (!reclass::is_enum_size(size))
        {
            error = tr("An enum is one, two, four or eight bytes wide.");
            return false;
        }

        description.name = name.toStdString();
        description.size = size;

        for (int row = 0; row < values_->rowCount(); ++row)
        {
            const QString value_name = cell_text(values_, row, kNameColumn).trimmed();
            if (value_name.isEmpty())
            {
                continue;
            }

            const QString    text  = cell_text(values_, row, kValueColumn).trimmed();
            bool             valid = false;
            const qulonglong value = text.isEmpty() ? 0 : text.toULongLong(&valid, 0);
            if (!text.isEmpty() && !valid)
            {
                error = tr("'%1' is not a number.").arg(text);
                return false;
            }
            if (size < 8 && (value >> (size * 8)) != 0)
            {
                error = tr("'%1' does not fit the enum's width.").arg(text);
                return false;
            }

            description.values.push_back(reclass::EnumValue {
                .name    = value_name.toStdString(),
                .value   = value,
                .comment = cell_text(values_, row, kCommentColumn).trimmed().toStdString(),
            });
        }
        return true;
    }

    bool EnumEditorDialog::apply()
    {
        reclass::EnumDescription description;
        QString                  error;
        if (!collect(description, error))
        {
            emit statusChanged(error, true);
            return false;
        }

        if (creating_)
        {
            reclass::EnumDescription* created = project_.add_enum(description.name, description.size);
            if (created == nullptr)
            {
                emit statusChanged(tr("An enum named '%1' already exists.").arg(to_qstring(description.name)), true);
                return false;
            }
            *created = std::move(description);
            applied_ = to_qstring(created->name);
            emit statusChanged(tr("Enum '%1' added.").arg(applied_), false);
            return true;
        }

        reclass::EnumDescription* existing = nullptr;
        for (reclass::EnumDescription& candidate : project_.enums())
        {
            if (candidate.name == target_name_.toStdString())
            {
                existing = &candidate;
            }
        }
        if (existing == nullptr)
        {
            emit statusChanged(tr("The enum is gone."), true);
            return false;
        }
        if (description.name != existing->name && project_.find_enum(description.name) != nullptr)
        {
            emit statusChanged(tr("An enum named '%1' already exists.").arg(to_qstring(description.name)), true);
            return false;
        }

        *existing = std::move(description);
        applied_  = to_qstring(existing->name);
        emit statusChanged(tr("Enum '%1' saved.").arg(applied_), false);
        return true;
    }

    QString EnumEditorDialog::applied_name() const
    {
        return applied_;
    }

    EnumChooserDialog::EnumChooserDialog(const reclass::Project& project, QString current, QWidget* parent)
        : QDialog(parent)
    {
        setWindowTitle(tr("Enum"));

        combo_ = new QComboBox(this);
        combo_->addItem(tr("(no enum)"), QString {});
        for (const reclass::EnumDescription& description : project.enums())
        {
            combo_->addItem(to_qstring(description.name), to_qstring(description.name));
        }
        const int index = combo_->findData(current);
        combo_->setCurrentIndex(index < 0 ? 0 : index);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        auto* layout = new QVBoxLayout(this);
        layout->addWidget(combo_);
        layout->addWidget(buttons);
    }

    QComboBox* EnumChooserDialog::combo() const noexcept
    {
        return combo_;
    }

    QString EnumChooserDialog::chosen_name() const
    {
        return combo_->currentData().toString();
    }
}
