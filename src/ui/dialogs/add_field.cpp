#include "ui/dialogs/add_field.hpp"

#include <optional>
#include <string>

#include "ui/components/widgets.hpp"
#include "ui/text.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

namespace slopkit::ui::dialogs
{
    namespace
    {
        struct KindOption
        {
            const char*        label;
            structs::FieldKind kind;
        };

        constexpr KindOption kKinds[] = {
            {           "Integer",   structs::FieldKind::integer},
            {    "Floating point",  structs::FieldKind::floating},
            {           "Pointer",   structs::FieldKind::pointer},
            { "Array of integers",     structs::FieldKind::array},
            {              "Text",      structs::FieldKind::text},
            {"Embedded structure", structs::FieldKind::structure},
        };

        [[nodiscard]] std::optional<std::uint64_t> parse_offset(const QString& text)
        {
            const QString trimmed = text.trimmed();
            if (trimmed.isEmpty())
            {
                return std::nullopt;
            }
            bool ok = false;
            if (trimmed.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
            {
                const qulonglong value = trimmed.mid(2).toULongLong(&ok, 16);
                return ok ? std::optional<std::uint64_t> {value} : std::nullopt;
            }
            const qulonglong value = trimmed.toULongLong(&ok, 10);
            return ok ? std::optional<std::uint64_t> {value} : std::nullopt;
        }
    }

    AddFieldDialog::AddFieldDialog(QWidget* parent) : QDialog(parent)
    {
        setWindowTitle(tr("Add Field"));
        setObjectName(QStringLiteral("add_field_dialog"));

        name_edit_ = new QLineEdit(this);
        name_edit_->setObjectName(QStringLiteral("field_name"));

        offset_edit_ = new QLineEdit(this);
        offset_edit_->setObjectName(QStringLiteral("field_offset"));
        offset_edit_->setPlaceholderText(QStringLiteral("0x10"));

        kind_combo_ = new widgets::ScrollingComboBox(6, this);
        kind_combo_->setObjectName(QStringLiteral("field_kind"));
        for (const KindOption& option : kKinds)
        {
            kind_combo_->addItem(tr(option.label), static_cast<int>(option.kind));
        }

        width_combo_ = new widgets::ScrollingComboBox(4, this);
        width_combo_->setObjectName(QStringLiteral("field_width"));
        for (const int width : {1, 2, 4, 8})
        {
            width_combo_->addItem(tr("%1 byte(s)").arg(width), width);
        }
        width_combo_->setCurrentIndex(2);

        signed_check_ = new QCheckBox(tr("Signed"), this);
        signed_check_->setObjectName(QStringLiteral("field_signed"));
        signed_check_->setChecked(true);

        count_spin_ = new QSpinBox(this);
        count_spin_->setObjectName(QStringLiteral("field_count"));
        count_spin_->setRange(1, 0x100000);
        count_spin_->setValue(1);

        wide_check_ = new QCheckBox(tr("UTF-16 (char16_t)"), this);
        wide_check_->setObjectName(QStringLiteral("field_wide"));

        struct_combo_ = new widgets::ScrollingComboBox(10, this);
        struct_combo_->setObjectName(QStringLiteral("field_struct"));

        status_ = new widgets::StatusLabel(this);
        status_->setObjectName(QStringLiteral("field_status"));

        auto* form = new QFormLayout;
        form->addRow(tr("Name"), name_edit_);
        form->addRow(tr("Offset"), offset_edit_);
        form->addRow(tr("Type"), kind_combo_);
        form->addRow(tr("Element width"), width_combo_);
        form->addRow(QString(), signed_check_);
        form->addRow(tr("Element count"), count_spin_);
        form->addRow(QString(), wide_check_);
        form->addRow(tr("Referenced struct"), struct_combo_);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

        auto* layout = new QVBoxLayout(this);
        layout->addLayout(form);
        layout->addWidget(status_);
        layout->addWidget(buttons);

        connect(kind_combo_,
                &QComboBox::currentIndexChanged,
                this,
                [this]
                {
                    apply_kind();
                });
        connect(name_edit_,
                &QLineEdit::textChanged,
                this,
                [this]
                {
                    validate();
                });
        connect(offset_edit_,
                &QLineEdit::textChanged,
                this,
                [this]
                {
                    validate();
                });
        connect(buttons, &QDialogButtonBox::accepted, this, &AddFieldDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        apply_kind();
        validate();
    }

    void AddFieldDialog::populate_structs(const structs::Structs& definitions, const QString& selected)
    {
        struct_combo_->clear();
        for (const std::string& name : definitions.struct_names())
        {
            struct_combo_->addItem(to_qstring(name), to_qstring(name));
        }
        if (!selected.isEmpty())
        {
            const int index = struct_combo_->findData(selected);
            if (index >= 0)
            {
                struct_combo_->setCurrentIndex(index);
            }
        }
    }

    void AddFieldDialog::begin_add(const structs::Structs& definitions,
                                   const QString&          default_name,
                                   std::uint64_t           default_offset)
    {
        setWindowTitle(tr("Add Field"));
        name_edit_->setText(default_name);
        offset_edit_->setText(QStringLiteral("0x%1").arg(QString::number(default_offset, 16).toUpper()));
        kind_combo_->setCurrentIndex(0);
        width_combo_->setCurrentIndex(2);
        signed_check_->setChecked(true);
        count_spin_->setValue(1);
        wide_check_->setChecked(false);
        populate_structs(definitions, {});
        apply_kind();
        validate();
    }

    void AddFieldDialog::begin_edit(const structs::Structs& definitions, const structs::StructNode& node)
    {
        setWindowTitle(tr("Edit Field"));
        name_edit_->setText(to_qstring(node.name));
        offset_edit_->setText(QStringLiteral("0x%1").arg(QString::number(node.offset, 16).toUpper()));

        const int kind_index = kind_combo_->findData(static_cast<int>(node.type.kind));
        kind_combo_->setCurrentIndex(kind_index >= 0 ? kind_index : 0);

        const int width_index = width_combo_->findData(static_cast<int>(node.type.width));
        width_combo_->setCurrentIndex(width_index >= 0 ? width_index : 2);
        signed_check_->setChecked(node.type.is_signed);
        count_spin_->setValue(static_cast<int>(node.type.count));
        wide_check_->setChecked(node.type.wide);
        populate_structs(definitions, to_qstring(node.type.structure));
        apply_kind();
        validate();
    }

    void AddFieldDialog::apply_kind()
    {
        const auto kind = static_cast<structs::FieldKind>(kind_combo_->currentData().toInt());
        width_combo_->setEnabled(kind == structs::FieldKind::integer || kind == structs::FieldKind::floating
                                 || kind == structs::FieldKind::array);
        signed_check_->setEnabled(kind == structs::FieldKind::integer || kind == structs::FieldKind::array);
        wide_check_->setEnabled(kind == structs::FieldKind::text);
        struct_combo_->setEnabled(kind == structs::FieldKind::pointer || kind == structs::FieldKind::structure);
        count_spin_->setEnabled(kind != structs::FieldKind::pointer || count_spin_->value() > 1);
    }

    void AddFieldDialog::validate()
    {
        const bool name_ok   = !name_edit_->text().trimmed().isEmpty();
        const bool offset_ok = parse_offset(offset_edit_->text()).has_value();
        if (!name_ok)
        {
            status_->set_status(widgets::StatusKind::warning, tr("A field needs a name."));
        }
        else if (!offset_ok)
        {
            status_->set_status(widgets::StatusKind::warning, tr("The offset must be a number, for example 0x10."));
        }
        else
        {
            status_->clear_status();
        }
    }

    std::optional<FieldDraft> AddFieldDialog::draft() const
    {
        const auto offset = parse_offset(offset_edit_->text());
        if (!offset.has_value())
        {
            return std::nullopt;
        }

        FieldDraft draft;
        draft.name   = name_edit_->text().trimmed();
        draft.offset = *offset;

        structs::FieldType type;
        type.kind      = static_cast<structs::FieldKind>(kind_combo_->currentData().toInt());
        type.width     = static_cast<std::size_t>(width_combo_->currentData().toInt());
        type.is_signed = signed_check_->isChecked();
        type.count     = static_cast<std::size_t>(count_spin_->value());
        type.wide      = wide_check_->isChecked();
        type.structure = struct_combo_->currentData().toString().toStdString();

        if (type.kind == structs::FieldKind::floating && type.width != 8)
        {
            type.width = 4;
        }
        if (draft.name.isEmpty())
        {
            return std::nullopt;
        }
        draft.type = type;
        return draft;
    }

    void AddFieldDialog::accept()
    {
        if (!draft().has_value())
        {
            validate();
            return;
        }
        QDialog::accept();
    }

    QLineEdit* AddFieldDialog::name_edit() const noexcept
    {
        return name_edit_;
    }

    QLineEdit* AddFieldDialog::offset_edit() const noexcept
    {
        return offset_edit_;
    }

    QComboBox* AddFieldDialog::kind_combo() const noexcept
    {
        return kind_combo_;
    }

    QComboBox* AddFieldDialog::width_combo() const noexcept
    {
        return width_combo_;
    }

    QCheckBox* AddFieldDialog::signed_check() const noexcept
    {
        return signed_check_;
    }

    QSpinBox* AddFieldDialog::count_spin() const noexcept
    {
        return count_spin_;
    }

    QCheckBox* AddFieldDialog::wide_check() const noexcept
    {
        return wide_check_;
    }

    QComboBox* AddFieldDialog::struct_combo() const noexcept
    {
        return struct_combo_;
    }

}
