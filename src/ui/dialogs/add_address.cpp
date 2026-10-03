#include "ui/dialogs/add_address.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "scan/types.hpp"
#include "scan/value.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"

namespace slopkit::ui::dialogs
{

    namespace
    {
        constexpr std::size_t kMaxSize = 4096;

        bool parse_size(std::string_view text, std::size_t& out)
        {
            if (text.empty())
            {
                return false;
            }
            std::uint64_t value     = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc {} || end != text.data() + text.size() || value == 0 || value > kMaxSize)
            {
                return false;
            }
            out = static_cast<std::size_t>(value);
            return true;
        }
    } // namespace

    AddAddressDialog::AddAddressDialog(table::AddressTable& table, QWidget* parent) : QDialog(parent), table_(table)
    {
        setWindowTitle(tr("Add Address"));
        setMinimumWidth(360);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        auto* form = new QFormLayout();
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

        description_edit_ = new QLineEdit(tr("New address"), this);
        form->addRow(tr("Description"), description_edit_);

        address_edit_ = new QLineEdit(this);
        address_edit_->setFont(mono_font());
        address_edit_->setPlaceholderText(QStringLiteral("0x1234"));
        form->addRow(tr("Address"), address_edit_);

        type_combo_ = new QComboBox(this);
        for (const char* name : scan::kValueTypeNames)
        {
            type_combo_->addItem(QString::fromUtf8(name));
        }
        type_combo_->setCurrentIndex(2); // 4 Bytes
        form->addRow(tr("Type"), type_combo_);

        size_row_         = new QWidget(this);
        auto* size_layout = new QHBoxLayout(size_row_);
        size_layout->setContentsMargins(0, 0, 0, 0);
        size_edit_ = new QLineEdit(QStringLiteral("32"), size_row_);
        size_edit_->setFont(mono_font());
        size_layout->addWidget(size_edit_);
        form->addRow(tr("Size (bytes)"), size_row_);

        layout->addLayout(form);

        hex_check_ = new QCheckBox(tr("Show as hex"), this);
        layout->addWidget(hex_check_);

        status_ = new widgets::StatusLabel(this);
        layout->addWidget(status_);

        auto* buttons      = new QHBoxLayout();
        auto* add_button   = new widgets::PrimaryButton(tr("Add"), this);
        auto* close_button = widgets::secondary_button(tr("Close"), this);
        buttons->addWidget(add_button);
        buttons->addWidget(close_button);
        buttons->addStretch(1);
        layout->addLayout(buttons);

        connect(add_button, &QPushButton::clicked, this, &AddAddressDialog::commit);
        connect(close_button, &QPushButton::clicked, this, &QDialog::close);
        connect(type_combo_,
                &QComboBox::currentIndexChanged,
                this,
                [this]
                {
                    update_size_row();
                });

        update_size_row();
    }

    void AddAddressDialog::update_size_row()
    {
        const auto type    = static_cast<scan::ValueType>(type_combo_->currentIndex());
        const bool dynamic = type == scan::ValueType::string || type == scan::ValueType::byte_array;
        size_row_->setVisible(dynamic);
    }

    void AddAddressDialog::commit()
    {
        log::debug(log::category::ui, "add address submitted");
        const auto address = scan::parse_address(address_edit_->text().toStdString());
        if (!address)
        {
            log::warning(log::category::ui, std::format("add address rejected: {}", address.error().message));
            status_->set_status(widgets::StatusKind::error,
                                tr("Address: %1").arg(QString::fromStdString(address.error().message)));
            return;
        }

        const auto  type    = static_cast<scan::ValueType>(type_combo_->currentIndex());
        const bool  dynamic = type == scan::ValueType::string || type == scan::ValueType::byte_array;
        std::size_t size    = scan::value_size(type);
        if (type == scan::ValueType::all)
        {
            size = 4;
        }
        if (dynamic && !parse_size(size_edit_->text().toStdString(), size))
        {
            log::warning(log::category::ui,
                         std::format("add address rejected: size {}", size_edit_->text().toStdString()));
            status_->set_status(widgets::StatusKind::error, tr("Size must be a number between 1 and 4096."));
            return;
        }

        table::AddressEntry entry;
        entry.description = description_edit_->text().toStdString();
        entry.address     = *address;
        entry.type        = type;
        entry.hex         = hex_check_->isChecked();
        entry.bytes.assign(size, std::byte {0});
        table_.add(std::move(entry));

        status_->set_status(widgets::StatusKind::info, tr("Address added."));
    }

} // namespace slopkit::ui::dialogs
