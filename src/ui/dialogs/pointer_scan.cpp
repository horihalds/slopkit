#include "ui/dialogs/pointer_scan.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <utility>

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableView>
#include <QVBoxLayout>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "expr/expression.hpp"
#include "expr/resolver.hpp"
#include "scan/value.hpp"
#include "ui/components/elided_tooltip_delegate.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"
#include "ui/text.hpp"

namespace slopkit::ui::dialogs
{
    namespace
    {
        constexpr std::size_t kDefaultMaxResults = 500;

        [[nodiscard]] QString format_offsets(std::span<const std::int64_t> offsets)
        {
            QString text;
            for (const std::int64_t offset : offsets)
            {
                if (!text.isEmpty())
                {
                    text += QStringLiteral(" → ");
                }
                text += offset < 0
                          ? QStringLiteral("-0x") + QString::number(static_cast<qulonglong>(-offset), 16).toUpper()
                          : QStringLiteral("+0x") + QString::number(static_cast<qulonglong>(offset), 16).toUpper();
            }
            return text;
        }
    }

    PointerScanDialog::PointerScanDialog(process::AccessWorker&   worker,
                                         process::AttachedTarget& target,
                                         script::SymbolTable&     symbols,
                                         QWidget*                 parent)
        : QDialog(parent), worker_(worker), target_(target), symbols_(symbols)
    {
        setWindowTitle(tr("Pointer Scan"));
        setWindowFlag(Qt::Window, true);
        resize(860, 520);
        // A companion window: it must not own the process lifetime, or closing
        // the shell would leave slopkit running.
        setAttribute(Qt::WA_QuitOnClose, false);
        build_layout();
        refresh();
    }

    void PointerScanDialog::build_layout()
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(6);

        layout->addWidget(widgets::section_header(tr("Pointer Scan"), this));

        auto* target_row = new QHBoxLayout();
        target_row->setSpacing(6);
        target_row->addWidget(new QLabel(tr("Target address"), this));
        target_edit_ = new QLineEdit(this);
        target_edit_->setObjectName(QStringLiteral("pointer_scan_target"));
        target_edit_->setFont(mono_font());
        target_edit_->setPlaceholderText(tr("module+0x1234, [module+0x10]+0x18, or 0x1234"));
        target_row->addWidget(target_edit_, 1);
        layout->addLayout(target_row);

        auto* options_row = new QHBoxLayout();
        options_row->setSpacing(6);
        options_row->addWidget(new QLabel(tr("Module"), this));
        module_combo_ = new QComboBox(this);
        module_combo_->setObjectName(QStringLiteral("pointer_scan_module"));
        module_combo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        options_row->addWidget(module_combo_, 1);

        options_row->addWidget(new QLabel(tr("Levels"), this));
        levels_spin_ = new QSpinBox(this);
        levels_spin_->setObjectName(QStringLiteral("pointer_scan_levels"));
        levels_spin_->setRange(1, 8);
        levels_spin_->setValue(3);
        options_row->addWidget(levels_spin_);

        options_row->addWidget(new QLabel(tr("Max offset"), this));
        offset_edit_ = new QLineEdit(QStringLiteral("0x1000"), this);
        offset_edit_->setObjectName(QStringLiteral("pointer_scan_max_offset"));
        offset_edit_->setFont(mono_font());
        offset_edit_->setMaximumWidth(120);
        options_row->addWidget(offset_edit_);

        negative_check_ = new QCheckBox(tr("Allow negative offsets"), this);
        negative_check_->setObjectName(QStringLiteral("pointer_scan_negative"));
        options_row->addWidget(negative_check_);

        scan_ = new widgets::PrimaryButton(tr("Scan"), this);
        scan_->setObjectName(QStringLiteral("pointer_scan_scan"));
        options_row->addWidget(scan_);
        layout->addLayout(options_row);

        hint_ = new widgets::StatusLabel(this);
        hint_->setObjectName(QStringLiteral("pointer_scan_hint"));
        hint_->setFont(mono_font());
        layout->addWidget(hint_);

        model_ = new models::PointerScanModel(this,
                                              [this](std::uint64_t address)
                                              {
                                                  return ui::format_cell_address(
                                                      ui::AddressMode::module_relative, spans_, address);
                                              });
        table_ = new QTableView(this);
        table_->setObjectName(QStringLiteral("pointer_scan_table"));
        table_->setModel(model_);
        table_->setItemDelegate(new widgets::ElidedTooltipDelegate(table_));
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_->setShowGrid(false);
        table_->verticalHeader()->setVisible(false);
        table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table_->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(table_, 1);

        empty_state_ = new QLabel(this);
        empty_state_->setObjectName(QStringLiteral("pointer_scan_empty"));
        empty_state_->setWordWrap(true);
        empty_state_->setAlignment(Qt::AlignCenter);
        empty_state_->setText(tr("No pointer paths yet. Enter a target address and select Scan."));
        layout->addWidget(empty_state_, 1);
        table_->setVisible(false);

        status_ = new widgets::StatusLabel(this);
        status_->setObjectName(QStringLiteral("pointer_scan_status"));
        layout->addWidget(status_);

        connect(scan_, &QPushButton::clicked, this, &PointerScanDialog::request_scan);
        connect(target_edit_, &QLineEdit::returnPressed, this, &PointerScanDialog::request_scan);
        connect(module_combo_, &QComboBox::currentIndexChanged, this, &PointerScanDialog::update_controls);
        connect(table_,
                &QTableView::doubleClicked,
                this,
                [this](const QModelIndex& index)
                {
                    if (index.isValid())
                    {
                        emit addressActivated(model_->base_at(index.row()));
                    }
                });

        widgets::chain_tab_order(
            {target_edit_, module_combo_, levels_spin_, offset_edit_, negative_check_, scan_, table_});
    }

    void PointerScanDialog::set_modules(std::vector<process::ModuleInfo> modules)
    {
        modules_ = std::move(modules);
        spans_.set_modules(modules_);

        const QString        previous = module_combo_->currentText();
        const QSignalBlocker blocker(module_combo_);
        module_combo_->clear();
        for (const process::ModuleInfo& module : modules_)
        {
            module_combo_->addItem(QString::fromStdString(module.name));
        }

        int index = module_combo_->findText(previous);
        if (index < 0)
        {
            for (std::size_t position = 0; position < modules_.size(); ++position)
            {
                if (modules_[position].is_main)
                {
                    index = static_cast<int>(position);
                    break;
                }
            }
        }
        if (index < 0 && !modules_.empty())
        {
            index = 0;
        }
        module_combo_->setCurrentIndex(index);
        update_controls();
    }

    void PointerScanDialog::refresh()
    {
        update_controls();
    }

    void PointerScanDialog::set_target(std::uint64_t address)
    {
        target_edit_->setText(ui::format_absolute(address));
        target_edit_->setFocus();
        target_edit_->selectAll();
    }

    const process::ModuleInfo* PointerScanDialog::selected_module() const
    {
        const int index = module_combo_->currentIndex();
        if (index < 0 || static_cast<std::size_t>(index) >= modules_.size())
        {
            return nullptr;
        }
        return &modules_[static_cast<std::size_t>(index)];
    }

    void PointerScanDialog::request_scan()
    {
        if (scan_job_.has_value() || resolve_job_.has_value())
        {
            return;
        }
        if (!target_.valid())
        {
            status_->set_status(widgets::StatusKind::warning, tr("Attach to a target first."));
            return;
        }
        const process::ModuleInfo* module = selected_module();
        if (module == nullptr)
        {
            status_->set_status(widgets::StatusKind::warning, tr("Pick a module before scanning."));
            return;
        }

        const std::string offset_text = offset_edit_->text().trimmed().toStdString();
        const auto        offset      = scan::parse_address(offset_text);
        if (!offset.has_value() || *offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        {
            status_->set_status(widgets::StatusKind::error,
                                tr("Max offset: %1").arg(tr("enter a non-negative offset such as 0x1000.")));
            return;
        }

        const std::string text       = target_edit_->text().trimmed().toStdString();
        const auto        expression = expr::parse(text);
        if (!expression)
        {
            status_->set_status(widgets::StatusKind::error,
                                tr("Target address: %1").arg(QString::fromStdString(expression.error().message)));
            return;
        }

        const std::int64_t max_offset = static_cast<std::int64_t>(*offset);
        const bool         negative   = negative_check_->isChecked();

        if (expression->pointer_levels() == 0)
        {
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
                status_->set_status(widgets::StatusKind::error,
                                    tr("Target address: %1").arg(QString::fromStdString(resolved.error().message)));
                return;
            }
            start_scan(*resolved, *module, max_offset, negative);
            return;
        }

        const process::JobId id = worker_.next_job_id();
        resolve_job_            = id;
        update_controls();
        status_->set_status(widgets::StatusKind::info, tr("Resolving the target address…"));

        const bool submitted =
            worker_.submit_resolve_expressions(id,
                                               std::vector<process::ResolveRequest> {
                                                   process::ResolveRequest {.key = 0, .expression = text}
        },
                                               ui::module_refs(spans_),
                                               module->pointer_size == 0 ? 8 : module->pointer_size,
                                               [this, id](process::JobResult&& result)
                                               {
                                                   finish_resolve(id, std::move(result));
                                               });
        if (!submitted)
        {
            resolve_job_.reset();
            update_controls();
            status_->set_status(widgets::StatusKind::warning, tr("Could not start the target resolution."));
        }
    }

    void PointerScanDialog::finish_resolve(process::JobId id, process::JobResult&& result)
    {
        if (resolve_job_ != id)
        {
            return;
        }
        resolve_job_.reset();
        update_controls();

        const auto& resolved = std::get<process::ResolveResult>(result);
        if (resolved.error.has_value())
        {
            status_->set_status(widgets::StatusKind::warning,
                                tr("Could not resolve the target address: %1")
                                    .arg(QString::fromStdString(std::string(describe(*resolved.error)))));
            return;
        }
        if (resolved.items.empty() || !resolved.items.front().address.has_value())
        {
            status_->set_status(
                widgets::StatusKind::warning,
                tr("Could not resolve the target address: %1")
                    .arg(QString::fromStdString(resolved.items.empty() ? "no result" : resolved.items.front().error)));
            return;
        }

        const process::ModuleInfo* module = selected_module();
        if (module == nullptr)
        {
            status_->set_status(widgets::StatusKind::warning, tr("Pick a module before scanning."));
            return;
        }

        const std::string offset_text = offset_edit_->text().trimmed().toStdString();
        const auto        offset      = scan::parse_address(offset_text);
        if (!offset.has_value() || *offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        {
            status_->set_status(widgets::StatusKind::error,
                                tr("Max offset: %1").arg(tr("enter a non-negative offset such as 0x1000.")));
            return;
        }
        start_scan(
            *resolved.items.front().address, *module, static_cast<std::int64_t>(*offset), negative_check_->isChecked());
    }

    void PointerScanDialog::start_scan(std::uint64_t              target,
                                       const process::ModuleInfo& module,
                                       std::int64_t               max_offset,
                                       bool                       negative)
    {
        const process::JobId id = worker_.next_job_id();
        scan_job_               = id;
        update_controls();

        status_->set_status(widgets::StatusKind::info,
                            tr("Scanning %1 for pointers to %2…")
                                .arg(QString::fromStdString(module.name), ui::format_absolute(target)));
        log::info(log::category::scan, std::format("pointer scan of {} for {:#x} started", module.name, target));

        const analysis::PointerScanConfig config {
            .levels         = static_cast<std::size_t>(levels_spin_->value()),
            .max_offset     = max_offset,
            .allow_negative = negative,
            .max_results    = kDefaultMaxResults,
            .pointer_size = module.pointer_size == 0 ? std::size_t {8} : static_cast<std::size_t>(module.pointer_size),
        };

        const bool submitted = worker_.submit_pointer_scan(id,
                                                           module.name,
                                                           module.base,
                                                           module.size,
                                                           target,
                                                           config,
                                                           [this, id](process::JobResult&& result)
                                                           {
                                                               finish_scan(id, std::move(result));
                                                           });
        if (!submitted)
        {
            scan_job_.reset();
            update_controls();
            status_->set_status(widgets::StatusKind::warning, tr("Could not start the pointer scan."));
        }
    }

    void PointerScanDialog::finish_scan(process::JobId id, process::JobResult&& result)
    {
        if (scan_job_ != id)
        {
            return;
        }
        scan_job_.reset();
        update_controls();

        const auto& scan = std::get<process::PointerScanResult>(result);
        if (scan.error.has_value())
        {
            status_->set_status(widgets::StatusKind::warning,
                                tr("Could not scan %1: %2")
                                    .arg(QString::fromStdString(scan.module),
                                         QString::fromStdString(std::string(describe(*scan.error)))));
            return;
        }

        std::vector<models::PointerScanRow> rows;
        rows.reserve(scan.paths.size());
        for (const analysis::PointerPath& path : scan.paths)
        {
            rows.push_back(models::PointerScanRow {.base = path.base, .offsets = format_offsets(path.offsets)});
        }
        const std::size_t count = rows.size();
        model_->set_rows(std::move(rows));
        table_->setVisible(count != 0);
        empty_state_->setVisible(count == 0);

        const QString reason = QString::fromStdString(analysis::describe(scan.report));
        if (count == 0)
        {
            empty_state_->setText(tr("No pointer paths were found for this target."));
            status_->set_status(
                widgets::StatusKind::info,
                tr("Pointer scan of %1 for %2 found 0 path(s). %3")
                    .arg(QString::fromStdString(scan.module), ui::format_absolute(scan.target), reason));
        }
        else
        {
            status_->set_status(widgets::StatusKind::info,
                                tr("Pointer scan of %1 for %2 found %3 path(s). %4")
                                    .arg(QString::fromStdString(scan.module), ui::format_absolute(scan.target))
                                    .arg(static_cast<int>(count))
                                    .arg(reason));
        }
        log::info(log::category::scan,
                  std::format("pointer scan of {} for {:#x} found {} path(s): {}",
                              scan.module,
                              scan.target,
                              count,
                              analysis::describe(scan.report)));
    }

    void PointerScanDialog::update_controls()
    {
        const bool busy           = scan_job_.has_value() || resolve_job_.has_value();
        const bool modules_loaded = !modules_.empty();
        const bool target_ready   = target_.valid();
        const bool ready          = target_ready && modules_loaded && !busy;

        scan_->setEnabled(ready);
        target_edit_->setEnabled(!busy);
        module_combo_->setEnabled(!busy && modules_loaded);
        levels_spin_->setEnabled(!busy);
        offset_edit_->setEnabled(!busy);
        negative_check_->setEnabled(!busy);

        if (!target_ready)
        {
            hint_->set_status(widgets::StatusKind::info, tr("Attach to a target to scan."));
        }
        else if (!modules_loaded)
        {
            hint_->set_status(widgets::StatusKind::info, tr("The target's memory map is not available yet."));
        }
        else if (busy)
        {
            hint_->set_status(widgets::StatusKind::info, tr("Scanning writable memory…"));
        }
        else
        {
            hint_->set_status(
                widgets::StatusKind::info,
                tr("Scans every writable region for chains ending in the selected module's data section."));
        }
    }

    QLineEdit* PointerScanDialog::target_edit() const noexcept
    {
        return target_edit_;
    }

    QComboBox* PointerScanDialog::module_combo() const noexcept
    {
        return module_combo_;
    }

    QSpinBox* PointerScanDialog::levels_spin() const noexcept
    {
        return levels_spin_;
    }

    QLineEdit* PointerScanDialog::offset_edit() const noexcept
    {
        return offset_edit_;
    }

    QCheckBox* PointerScanDialog::negative_check() const noexcept
    {
        return negative_check_;
    }

    QPushButton* PointerScanDialog::scan_button() const noexcept
    {
        return scan_;
    }

    QTableView* PointerScanDialog::table() const noexcept
    {
        return table_;
    }

    QLabel* PointerScanDialog::empty_state() const noexcept
    {
        return empty_state_;
    }

    QString PointerScanDialog::status_text() const
    {
        return status_->text();
    }

    int PointerScanDialog::row_count() const
    {
        return model_->row_count();
    }

    std::uint64_t PointerScanDialog::base_at(int row) const
    {
        return model_->base_at(row);
    }

    QString PointerScanDialog::cell_text(int row, int column) const
    {
        const QModelIndex index = model_->index(row, column);
        return index.isValid() ? model_->data(index, Qt::DisplayRole).toString() : QString();
    }
}
