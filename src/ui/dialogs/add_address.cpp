#include "ui/dialogs/add_address.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
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
#include <QShowEvent>
#include <QVBoxLayout>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "scan/types.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"

namespace slopkit::ui::dialogs
{

    namespace
    {
        constexpr std::size_t kMaxSize          = 4096;
        constexpr int         kDefaultTypeIndex = static_cast<int>(scan::ValueType::int32); // 4 Bytes

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

    AddAddressDialog::AddAddressDialog(table::AddressTable& table, process::AccessWorker& worker, QWidget* parent)
        : QDialog(parent), table_(table), worker_(worker)
    {
        setWindowTitle(tr("Add Address"));
        setMinimumWidth(360);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        auto* form = new QFormLayout();
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

        description_edit_ = new QLineEdit(this);
        description_edit_->setObjectName(QStringLiteral("description_edit"));
        form->addRow(tr("Description"), description_edit_);

        address_edit_ = new QLineEdit(this);
        address_edit_->setObjectName(QStringLiteral("address_edit"));
        address_edit_->setFont(mono_font());
        address_edit_->setPlaceholderText(QStringLiteral("firefox-bin+4096"));
        form->addRow(tr("Address"), address_edit_);

        type_combo_ = new QComboBox(this);
        type_combo_->setObjectName(QStringLiteral("type_combo"));
        for (const char* name : scan::kValueTypeNames)
        {
            type_combo_->addItem(QString::fromUtf8(name));
        }
        form->addRow(tr("Type"), type_combo_);

        size_row_         = new QWidget(this);
        auto* size_layout = new QHBoxLayout(size_row_);
        size_layout->setContentsMargins(0, 0, 0, 0);
        size_edit_ = new QLineEdit(size_row_);
        size_edit_->setObjectName(QStringLiteral("size_edit"));
        size_edit_->setFont(mono_font());
        size_layout->addWidget(size_edit_);
        form->addRow(tr("Size (bytes)"), size_row_);

        layout->addLayout(form);

        hex_check_ = new QCheckBox(tr("Show as hex"), this);
        hex_check_->setObjectName(QStringLiteral("hex_check"));
        layout->addWidget(hex_check_);

        status_ = new widgets::StatusLabel(this);
        layout->addWidget(status_);

        auto* buttons      = new QHBoxLayout();
        add_button_        = new widgets::PrimaryButton(tr("Add"), this);
        auto* close_button = widgets::secondary_button(tr("Close"), this);
        buttons->addWidget(add_button_);
        buttons->addWidget(close_button);
        buttons->addStretch(1);
        layout->addLayout(buttons);

        connect(add_button_, &QPushButton::clicked, this, &AddAddressDialog::commit);
        connect(close_button, &QPushButton::clicked, this, &QDialog::close);
        connect(type_combo_,
                &QComboBox::currentIndexChanged,
                this,
                [this]
                {
                    update_size_row();
                });

        reset_form();
    }

    void AddAddressDialog::set_modules(std::vector<process::ModuleInfo> modules)
    {
        spans_.set_modules(modules);
        // A new memory map means new module bases, so an in-flight resolve is stale.
        resolve_job_.reset();
        if (add_button_ != nullptr)
        {
            add_button_->setEnabled(true);
            add_button_->setText(tr("Add"));
        }
    }

    void AddAddressDialog::showEvent(QShowEvent* event)
    {
        QDialog::showEvent(event);
        // Every opening starts from a clean form, so a previous entry is never
        // submitted twice by accident and a late completion is inert.
        reset_form();
    }

    void AddAddressDialog::update_size_row()
    {
        const auto type    = static_cast<scan::ValueType>(type_combo_->currentIndex());
        const bool dynamic = type == scan::ValueType::string || type == scan::ValueType::byte_array;
        size_row_->setVisible(dynamic);
    }

    void AddAddressDialog::reset_form()
    {
        resolve_job_.reset();
        if (add_button_ != nullptr)
        {
            add_button_->setEnabled(true);
            add_button_->setText(tr("Add"));
        }

        description_edit_->setText(tr("New address"));
        address_edit_->clear();
        type_combo_->setCurrentIndex(kDefaultTypeIndex);
        size_edit_->setText(QStringLiteral("32"));
        hex_check_->setChecked(false);
        status_->clear_status();
        update_size_row();
    }

    void AddAddressDialog::commit()
    {
        log::debug(log::category::ui, "add address submitted");

        const std::string text       = address_edit_->text().toStdString();
        const auto        expression = expr::parse(text);
        if (!expression)
        {
            reject_address(QString::fromStdString(expression.error().message));
            return;
        }

        if (expression->pointer_levels() == 0)
        {
            // At most one offset: resolve it here. The reader is never reached.
            const auto resolved =
                expr::evaluate(*expression,
                               ui::module_refs(spans_),
                               [](std::uint64_t) -> std::expected<std::uint64_t, std::string>
                               {
                                   return std::unexpected(std::string {"a pointer read was required"});
                               });
            if (!resolved)
            {
                reject_address(QString::fromStdString(resolved.error().message));
                return;
            }
            add_entry(*resolved, text);
            return;
        }

        begin_resolve(text);
    }

    void AddAddressDialog::reject_address(const QString& message)
    {
        log::warning(log::category::ui, std::format("add address rejected: {}", message.toStdString()));
        status_->set_status(widgets::StatusKind::error, tr("Address: %1").arg(message));
    }

    void AddAddressDialog::begin_resolve(std::string expression)
    {
        const process::JobId id = worker_.next_job_id();
        resolve_job_            = id;
        add_button_->setEnabled(false);
        add_button_->setText(tr("Resolving..."));
        status_->set_status(widgets::StatusKind::info, tr("Resolving..."));

        const bool submitted =
            worker_.submit_resolve_expressions(id,
                                               std::vector<process::ResolveRequest> {
                                                   process::ResolveRequest {.key = 0, .expression = expression}
        },
                                               ui::module_refs(spans_),
                                               8,
                                               [this, id, expression](process::JobResult&& result)
                                               {
                                                   finish_resolve(id, expression, std::move(result));
                                               });

        if (!submitted)
        {
            resolve_job_.reset();
            add_button_->setEnabled(true);
            add_button_->setText(tr("Add"));
            reject_address(tr("the resolve worker is not accepting jobs"));
        }
    }

    void AddAddressDialog::finish_resolve(process::JobId id, const std::string& expression, process::JobResult&& result)
    {
        if (resolve_job_ != id)
        {
            return; // a later opening superseded this resolve
        }
        resolve_job_.reset();
        add_button_->setEnabled(true);
        add_button_->setText(tr("Add"));

        const auto& resolved = std::get<process::ResolveResult>(result);
        if (resolved.error.has_value())
        {
            reject_address(tr("a target is required for a pointer expression"));
            return;
        }
        if (resolved.items.empty() || !resolved.items.front().address.has_value())
        {
            reject_address(resolved.items.empty() ? tr("the expression could not be resolved")
                                                  : QString::fromStdString(resolved.items.front().error));
            return;
        }
        add_entry(*resolved.items.front().address, expression);
    }

    void AddAddressDialog::add_entry(std::uint64_t address, const std::string& expression)
    {
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
        entry.address     = address;
        entry.expression  = expression;
        entry.type        = type;
        entry.hex         = hex_check_->isChecked();
        entry.bytes.assign(size, std::byte {0});
        table_.add(std::move(entry));

        // A confirmed add closes the dialog; the new row is the feedback.
        log::info(log::category::ui, "add address accepted");
        accept();
    }

} // namespace slopkit::ui::dialogs
