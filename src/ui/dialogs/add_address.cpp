#include "ui/dialogs/add_address.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPushButton>
#include <QShowEvent>
#include <QTimer>
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
        constexpr std::size_t kMaxSize          = 4096;
        constexpr int         kDefaultTypeIndex = static_cast<int>(scan::ValueType::int32); // 4 Bytes
        constexpr int         kPreviewDelayMs   = 250;

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

    AddAddressDialog::AddAddressDialog(table::AddressTable&   table,
                                       process::AccessWorker& worker,
                                       QWidget*               parent,
                                       script::SymbolTable&   symbols)
        : QDialog(parent), table_(table), worker_(worker), symbols_(symbols)
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

        preview_label_ = new QLabel(this);
        preview_label_->setObjectName(QStringLiteral("preview_label"));
        preview_label_->setFont(mono_font());
        preview_label_->setWordWrap(true);
        {
            // A muted hint, so an unresolved note reads like information and not
            // an error; the colour comes from the theme through the status colour.
            QPalette preview_palette = preview_label_->palette();
            preview_palette.setColor(QPalette::WindowText, widgets::status_color(widgets::StatusKind::info));
            preview_label_->setPalette(preview_palette);
        }
        form->addRow(preview_label_);

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
                    on_address_changed();
                });
        connect(address_edit_,
                &QLineEdit::textChanged,
                this,
                [this]
                {
                    on_address_changed();
                });
        connect(size_edit_,
                &QLineEdit::textChanged,
                this,
                [this]
                {
                    on_address_changed();
                });
        connect(hex_check_,
                &QCheckBox::toggled,
                this,
                [this]
                {
                    // The bytes are unchanged, so a toggle only re-formats them.
                    refresh_preview();
                });

        // One-shot debounce: the preview resolves only once the user pauses, so
        // typing a bracketed chain does not submit a job per keystroke. It is not
        // periodic, so the app keeps its single timer (the live tick).
        preview_timer_ = new QTimer(this);
        preview_timer_->setSingleShot(true);
        preview_timer_->setInterval(kPreviewDelayMs);
        connect(preview_timer_,
                &QTimer::timeout,
                this,
                [this]
                {
                    resolve_now(address_edit_->text().toStdString());
                });

        reset_form();
    }

    void AddAddressDialog::set_modules(std::vector<process::ModuleInfo> modules)
    {
        spans_.set_modules(modules);
        // A new memory map means new module bases, so any in-flight resolve or
        // read is stale and the current text is re-resolved against the new map.
        clear_preview();
        if (add_button_ != nullptr)
        {
            add_button_->setEnabled(true);
            add_button_->setText(tr("Add"));
        }
        if (address_edit_ != nullptr && !address_edit_->text().isEmpty())
        {
            resolve_now(address_edit_->text().toStdString());
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

        // The setText() calls above fire the change signals; clear last so the
        // preview is left empty and its debounce timer stopped.
        pending_add_.reset();
        clear_preview();
    }

    void AddAddressDialog::commit()
    {
        log::debug(log::category::ui, "add address submitted");

        const std::string text       = address_edit_->text().toStdString();
        const auto        expression = expr::parse(text);
        if (!expression)
        {
            // Only a malformed expression is refused; a well-formed one that does
            // not resolve is added unresolved (see the plan's FR1/FR2).
            reject_address(QString::fromStdString(expression.error().message));
            return;
        }

        if (preview_.text == text && preview_.state == PreviewState::resolved)
        {
            // Reuse the live preview so a settled expression adds at once.
            add_entry(preview_.address, text);
            return;
        }

        pending_add_ = text;
        resolve_now(text);
    }

    void AddAddressDialog::reject_address(const QString& message)
    {
        log::warning(log::category::ui, std::format("add address rejected: {}", message.toStdString()));
        status_->set_status(widgets::StatusKind::error, tr("Address: %1").arg(message));
    }

    void AddAddressDialog::on_address_changed()
    {
        if (preview_timer_ != nullptr)
        {
            preview_timer_->start();
        }
    }

    void AddAddressDialog::resolve_now(const std::string& text)
    {
        if (preview_timer_ != nullptr)
        {
            preview_timer_->stop();
        }

        if (text.empty())
        {
            clear_preview();
            return;
        }

        // A resolve already in flight for exactly this text is reused, so an Add
        // click on top of a preview resolve does not submit a second job.
        if (preview_.resolve_job.has_value() && preview_.text == text)
        {
            if (pending_add_.has_value())
            {
                add_button_->setEnabled(false);
                add_button_->setText(tr("Resolving..."));
            }
            return;
        }

        // An edit (a resolve for a different text) abandons a pending Add: the
        // user has moved on from the text they confirmed.
        if (pending_add_.has_value() && *pending_add_ != text)
        {
            pending_add_.reset();
            add_button_->setEnabled(true);
            add_button_->setText(tr("Add"));
        }

        const auto expression = expr::parse(text);
        if (!expression)
        {
            set_preview_unresolved(text, QString::fromStdString(expression.error().message));
            return;
        }

        if (expression->pointer_levels() == 0)
        {
            // At most one offset: resolve it here. The reader is never reached, so
            // no target access happens on the UI thread.
            const auto resolved =
                expr::evaluate(*expression,
                               ui::module_refs(spans_),
                               symbols_.snapshot(),
                               [](std::uint64_t) -> std::expected<std::uint64_t, std::string>
                               {
                                   return std::unexpected(std::string {"a pointer read was required"});
                               });
            if (!resolved)
            {
                set_preview_unresolved(text, QString::fromStdString(resolved.error().message));
                return;
            }
            set_preview_resolved(text, *resolved);
            return;
        }

        // A bracketed/pointer chain is always resolved live by the worker.
        preview_.text    = text;
        preview_.state   = PreviewState::resolving;
        preview_.address = 0;
        preview_.value.reset();
        preview_.reason.clear();
        preview_.read_job.reset();
        refresh_preview();

        const process::JobId id = worker_.next_job_id();
        preview_.resolve_job    = id;
        if (pending_add_.has_value())
        {
            add_button_->setEnabled(false);
            add_button_->setText(tr("Resolving..."));
        }

        const bool submitted =
            worker_.submit_resolve_expressions(id,
                                               std::vector<process::ResolveRequest> {
                                                   process::ResolveRequest {.key = 0, .expression = text}
        },
                                               ui::module_refs(spans_),
                                               8,
                                               [this, id, text](process::JobResult&& result)
                                               {
                                                   finish_resolve(id, text, std::move(result));
                                               });

        if (!submitted)
        {
            preview_.resolve_job.reset();
            add_button_->setEnabled(true);
            add_button_->setText(tr("Add"));
            set_preview_unresolved(text, tr("the resolve worker is not accepting jobs"));
        }
    }

    void AddAddressDialog::finish_resolve(process::JobId id, const std::string& text, process::JobResult&& result)
    {
        if (!preview_.resolve_job.has_value() || *preview_.resolve_job != id)
        {
            return; // a later edit, opening or module map superseded this resolve
        }
        preview_.resolve_job.reset();
        if (pending_add_.has_value())
        {
            add_button_->setEnabled(true);
            add_button_->setText(tr("Add"));
        }

        const auto& resolved = std::get<process::ResolveResult>(result);
        if (resolved.error.has_value())
        {
            set_preview_unresolved(text, tr("a target is required"));
            return;
        }
        if (resolved.items.empty() || !resolved.items.front().address.has_value())
        {
            set_preview_unresolved(text,
                                   resolved.items.empty() ? tr("not resolved yet")
                                                          : QString::fromStdString(resolved.items.front().error));
            return;
        }
        set_preview_resolved(text, *resolved.items.front().address);
    }

    void AddAddressDialog::set_preview_resolved(const std::string& text, std::uint64_t address)
    {
        preview_.text    = text;
        preview_.state   = PreviewState::resolved;
        preview_.address = address;
        preview_.value.reset();
        preview_.reason.clear();
        preview_.read_job.reset();
        refresh_preview();

        if (pending_add_.has_value() && *pending_add_ == text)
        {
            pending_add_.reset();
            add_entry(address, text);
            return;
        }

        // The value read is target access, so it goes through the worker and only
        // while a target is attached.
        if (worker_.attached())
        {
            if (const auto size = preview_value_size(); size.has_value())
            {
                request_value_read(address, *size);
            }
        }
    }

    void AddAddressDialog::set_preview_unresolved(const std::string& text, const QString& reason)
    {
        preview_.text    = text;
        preview_.state   = PreviewState::unresolved;
        preview_.address = 0;
        preview_.value.reset();
        preview_.read_job.reset();
        preview_.reason = reason.toStdString();
        refresh_preview();
        log::debug(log::category::ui, std::format("add address preview unresolved: {}", preview_.reason));

        if (pending_add_.has_value() && *pending_add_ == text)
        {
            pending_add_.reset();
            // A resolve failure never blocks the add: the expression is kept and
            // the address stays unresolved (0) until the list re-resolves it.
            log::info(log::category::ui,
                      std::format("add address accepted unresolved: {} ({})", text, preview_.reason));
            add_entry(0, text);
        }
    }

    void AddAddressDialog::request_value_read(std::uint64_t address, std::size_t size)
    {
        preview_.value.reset();

        const process::JobId id = worker_.next_job_id();
        preview_.read_job       = id;

        const bool submitted = worker_.submit_read_many(id,
                                                        std::vector<process::ReadManyItem> {
                                                            process::ReadManyItem {.address = address, .size = size}
        },
                                                        [this, id](process::JobResult&& result)
                                                        {
                                                            apply_value_read(id, std::move(result));
                                                        });
        if (!submitted)
        {
            preview_.read_job.reset();
            refresh_preview();
        }
    }

    void AddAddressDialog::apply_value_read(process::JobId id, process::JobResult&& result)
    {
        if (!preview_.read_job.has_value() || *preview_.read_job != id)
        {
            return; // a later edit or resolve superseded this read
        }
        preview_.read_job.reset();

        const auto& reads = std::get<process::ReadManyResult>(result);
        if (!reads.items.empty() && reads.items.front().has_value())
        {
            preview_.value = *reads.items.front();
        }
        refresh_preview();
    }

    std::optional<std::size_t> AddAddressDialog::preview_value_size() const
    {
        const auto type = static_cast<scan::ValueType>(type_combo_->currentIndex());
        if (const std::size_t fixed = scan::value_size(type); fixed != 0)
        {
            return fixed;
        }

        std::size_t parsed = 0;
        if (!parse_size(size_edit_->text().toStdString(), parsed))
        {
            return std::nullopt;
        }
        return parsed;
    }

    void AddAddressDialog::refresh_preview()
    {
        if (preview_label_ == nullptr)
        {
            return;
        }

        switch (preview_.state)
        {
        case PreviewState::idle:
            preview_label_->clear();
            return;
        case PreviewState::resolving:
            preview_label_->setText(tr("resolving..."));
            return;
        case PreviewState::unresolved:
            preview_label_->setText(preview_.reason.empty() ? tr("not resolved yet")
                                                            : QString::fromStdString(preview_.reason));
            return;
        case PreviewState::resolved:
            break;
        }

        const QString address_text =
            ui::format_cell_address(ui::AddressMode::module_relative, spans_, preview_.address);

        QString value_text;
        if (preview_.value.has_value())
        {
            const auto type = static_cast<scan::ValueType>(type_combo_->currentIndex());
            value_text = QString::fromStdString(scan::format_value(type, *preview_.value, hex_check_->isChecked()));
        }
        else if (!worker_.attached())
        {
            value_text = tr("a target is required");
        }
        else
        {
            value_text = QStringLiteral("?");
        }

        preview_label_->setText(QStringLiteral("%1 = %2").arg(address_text, value_text));
    }

    void AddAddressDialog::clear_preview()
    {
        if (preview_timer_ != nullptr)
        {
            preview_timer_->stop();
        }
        preview_ = Preview {};
        if (preview_label_ != nullptr)
        {
            preview_label_->clear();
        }
    }

    void AddAddressDialog::add_entry(std::uint64_t address, const std::string& expression)
    {
        const auto  type    = static_cast<scan::ValueType>(type_combo_->currentIndex());
        const bool  dynamic = type == scan::ValueType::string || type == scan::ValueType::byte_array;
        std::size_t size    = scan::value_size(type);
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
