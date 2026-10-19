#include "ui/dialogs/code_filter.hpp"

#include <format>
#include <utility>

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableView>
#include <QVBoxLayout>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "ui/components/elided_tooltip_delegate.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"
#include "ui/text.hpp"

namespace slopkit::ui::dialogs
{
    namespace
    {
        QString kind_name(analysis::CodeTargetKind kind)
        {
            switch (kind)
            {
            case analysis::CodeTargetKind::call:
                return QStringLiteral("Call");
            case analysis::CodeTargetKind::jump:
                return QStringLiteral("Jump");
            case analysis::CodeTargetKind::conditional_jump:
                return QStringLiteral("Conditional jump");
            }
            return {};
        }
    }

    CodeFilterDialog::CodeFilterDialog(debug::Controller& controller, process::AttachedTarget& target)
        : QDialog(nullptr), controller_(controller), gate_(controller, target, *this, this)
    {
        setWindowTitle(tr("Code Filter"));
        setWindowFlag(Qt::Window, true);
        resize(780, 520);
        // A companion window: it must not own the process lifetime, or closing
        // the shell would leave slopkit running.
        setAttribute(Qt::WA_QuitOnClose, false);
        build_layout();

        connect(&controller_, &debug::Controller::codeFilterChanged, this, &CodeFilterDialog::refresh);
        connect(&controller_, &debug::Controller::stateChanged, this, &CodeFilterDialog::refresh);
        connect(&gate_,
                &DebugSessionGate::progress,
                this,
                [this](const QString& text, bool error)
                {
                    status_->set_status(error ? widgets::StatusKind::warning : widgets::StatusKind::info, text);
                });
        refresh();
    }

    void CodeFilterDialog::build_layout()
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(6);

        const ui::AddressText address_text = [this](std::uint64_t address)
        {
            return ui::format_cell_address(controller_.address_mode(), controller_.modules(), address);
        };

        header_ = new QLabel(this);
        header_->setObjectName(QStringLiteral("code_filter_header"));
        header_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        header_->setFont(mono_font());
        layout->addWidget(header_);

        auto* module_row = new QHBoxLayout();
        module_row->addWidget(new QLabel(tr("Module"), this));
        module_combo_ = new QComboBox(this);
        module_combo_->setObjectName(QStringLiteral("code_filter_module"));
        module_combo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        module_row->addWidget(module_combo_, 1);

        call_        = new QCheckBox(tr("Call"), this);
        jump_        = new QCheckBox(tr("Jump"), this);
        conditional_ = new QCheckBox(tr("Conditional jump"), this);
        call_->setObjectName(QStringLiteral("code_filter_call"));
        jump_->setObjectName(QStringLiteral("code_filter_jump"));
        conditional_->setObjectName(QStringLiteral("code_filter_conditional_jump"));
        call_->setChecked(true);
        module_row->addWidget(call_);
        module_row->addWidget(jump_);
        module_row->addWidget(conditional_);

        collect_ = widgets::secondary_button(tr("Collect"), this);
        collect_->setObjectName(QStringLiteral("code_filter_collect"));
        module_row->addWidget(collect_);
        layout->addLayout(module_row);

        status_ = new widgets::StatusLabel(this);
        status_->setObjectName(QStringLiteral("code_filter_status"));
        layout->addWidget(status_);

        model_ = new models::CodeFilterModel(this, address_text);
        table_ = new QTableView(this);
        table_->setObjectName(QStringLiteral("code_filter_table"));
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

        connect(table_,
                &QTableView::activated,
                this,
                [this](const QModelIndex& index)
                {
                    if (index.isValid())
                    {
                        emit targetActivated(model_->target_at(index.row()));
                    }
                });

        auto* buttons      = new QHBoxLayout();
        keep_executed_     = widgets::secondary_button(tr("Keep executed"), this);
        keep_not_executed_ = widgets::secondary_button(tr("Keep not executed"), this);
        clear_             = widgets::secondary_button(tr("Clear"), this);
        keep_executed_->setObjectName(QStringLiteral("code_filter_keep_executed"));
        keep_not_executed_->setObjectName(QStringLiteral("code_filter_keep_not_executed"));
        clear_->setObjectName(QStringLiteral("code_filter_clear"));
        buttons->addWidget(keep_executed_);
        buttons->addWidget(keep_not_executed_);
        buttons->addStretch(1);
        buttons->addWidget(clear_);
        layout->addLayout(buttons);

        hint_ = new widgets::StatusLabel(this);
        hint_->setObjectName(QStringLiteral("code_filter_hint"));
        hint_->setFont(mono_font());
        layout->addWidget(hint_);

        connect(collect_,
                &QPushButton::clicked,
                this,
                [this]
                {
                    gate_.with_session(tr("collect code targets for the filter"),
                                       [this]
                                       {
                                           request_collect();
                                       });
                });
        connect(keep_executed_,
                &QPushButton::clicked,
                this,
                [this]
                {
                    controller_.filter_code_targets(true);
                });
        connect(keep_not_executed_,
                &QPushButton::clicked,
                this,
                [this]
                {
                    controller_.filter_code_targets(false);
                });
        connect(clear_,
                &QPushButton::clicked,
                this,
                [this]
                {
                    controller_.stop_code_filter();
                });

        widgets::chain_tab_order(
            {module_combo_, call_, jump_, conditional_, collect_, table_, keep_executed_, keep_not_executed_, clear_});
        update_controls();
    }

    void CodeFilterDialog::set_modules(std::vector<process::ModuleInfo> modules)
    {
        modules_ = std::move(modules);

        const QString        previous = module_combo_->currentText();
        const QSignalBlocker blocker(module_combo_);
        module_combo_->clear();
        for (const process::ModuleInfo& module : modules_)
        {
            module_combo_->addItem(QString::fromStdString(module.name));
        }

        int index = -1;
        if (controller_.state() == debug::Controller::State::stopped)
        {
            const std::uint64_t stop = controller_.last_stop().trap_address;
            for (std::size_t i = 0; i < modules_.size(); ++i)
            {
                if (stop >= modules_[i].base && stop < modules_[i].base + modules_[i].size)
                {
                    index = static_cast<int>(i);
                    break;
                }
            }
        }
        if (index < 0)
        {
            index = module_combo_->findText(previous);
        }
        if (index < 0 && !modules_.empty())
        {
            index = 0;
        }
        module_combo_->setCurrentIndex(index);
        update_controls();
    }

    const process::ModuleInfo* CodeFilterDialog::selected_module() const
    {
        const int index = module_combo_->currentIndex();
        if (index < 0 || static_cast<std::size_t>(index) >= modules_.size())
        {
            return nullptr;
        }
        return &modules_[static_cast<std::size_t>(index)];
    }

    void CodeFilterDialog::request_collect()
    {
        const process::ModuleInfo* module = selected_module();
        if (module == nullptr)
        {
            status_->set_status(widgets::StatusKind::warning, tr("Pick a module before collecting."));
            return;
        }
        const std::string               name         = module->name;
        const std::uint64_t             base         = module->base;
        const std::uint64_t             end          = module->base + module->size;
        const std::uint32_t             pointer_size = module->pointer_size;
        const analysis::CodeTargetKinds kinds {
            .call = call_->isChecked(), .jump = jump_->isChecked(), .conditional_jump = conditional_->isChecked()};

        slopkit::log::info(log::category::ui,
                           std::format("code filter collecting {} from {:#x}-{:#x}", name, base, end));
        status_->set_status(widgets::StatusKind::info, tr("Scanning %1…").arg(QString::fromStdString(name)));
        emit collectRequested(QString::fromStdString(name), base, end, pointer_size, kinds);
    }

    void CodeFilterDialog::apply_scan(analysis::CodeTargetScanResult scan, std::string module, std::uint64_t base)
    {
        module_name_ = std::move(module);
        module_base_ = base;
        kind_by_target_.clear();

        std::vector<debug::CodeFilterCandidate> candidates;
        candidates.reserve(scan.targets.size());
        for (const analysis::CodeTarget& target : scan.targets)
        {
            kind_by_target_[target.target] = target.kind;
            debug::CodeFilterCandidate candidate;
            candidate.target      = target.target;
            candidate.instruction = target.instruction;
            candidates.push_back(candidate);
        }

        summary_ = QString::fromStdString(analysis::describe(scan.report));
        slopkit::log::info(
            log::category::debug,
            std::format("code filter collected {} candidate(s) from {}", candidates.size(), module_name_));
        controller_.start_code_filter(std::move(candidates));
        refresh();
    }

    void CodeFilterDialog::report_error(const QString& text)
    {
        status_->set_status(widgets::StatusKind::warning, text);
        slopkit::log::warning(log::category::ui, std::format("code filter: {}", text.toStdString()));
    }

    void CodeFilterDialog::refresh()
    {
        rebuild_rows();

        const debug::CodeFilter& filter = controller_.code_filter();
        if (filter.state() == debug::CodeFilterState::idle)
        {
            header_->setText(tr("No candidates collected."));
            if (!filter.reset_reason().isEmpty())
            {
                status_->set_status(widgets::StatusKind::info, tr("Filter cleared: %1").arg(filter.reset_reason()));
            }
        }
        else
        {
            header_->setText(tr("%1 candidate(s) · %2 armed · %3 waiting · %4 executed · %5 dropped")
                                 .arg(filter.visible_count())
                                 .arg(filter.armed_count())
                                 .arg(filter.waiting_count())
                                 .arg(filter.executed_count())
                                 .arg(filter.dropped_count()));
            if (!summary_.isEmpty())
            {
                status_->set_status(widgets::StatusKind::info, summary_);
            }
            else
            {
                status_->clear_status();
            }
        }

        update_controls();
    }

    void CodeFilterDialog::update_controls()
    {
        const bool active = controller_.code_filter().state() != debug::CodeFilterState::idle;

        collect_->setEnabled(!modules_.empty());
        keep_executed_->setEnabled(active);
        keep_not_executed_->setEnabled(active);
        clear_->setEnabled(active);

        if (modules_.empty())
        {
            hint_->set_status(widgets::StatusKind::info, tr("Attach to a target to collect."));
        }
        else if (active)
        {
            hint_->set_status(
                widgets::StatusKind::info,
                tr("Filter the list with the Keep buttons; the filter keeps collecting while the target runs."));
        }
        else
        {
            hint_->set_status(widgets::StatusKind::info,
                              tr("Collect while the target runs; hits mark themselves executed."));
        }
    }

    void CodeFilterDialog::rebuild_rows()
    {
        std::vector<models::CodeFilterRow> rows;
        for (const debug::CodeFilterCandidate& candidate : controller_.code_filter().candidates())
        {
            if (!candidate.visible)
            {
                continue;
            }
            const auto kind = kind_by_target_.find(candidate.target);
            rows.push_back(models::CodeFilterRow {
                .target        = candidate.target,
                .instruction   = candidate.instruction,
                .module_offset = module_name_.empty()
                                   ? QString()
                                   : QString::fromStdString(
                                         std::format("{}+0x{:X}", module_name_, candidate.target - module_base_)),
                .kind       = kind_name(kind != kind_by_target_.end() ? kind->second : analysis::CodeTargetKind::call),
                .executed   = candidate.executions > 0,
                .executions = candidate.executions,
            });
        }
        model_->set_rows(std::move(rows));
    }

    DebugSessionGate& CodeFilterDialog::debug_gate() noexcept
    {
        return gate_;
    }

    QString CodeFilterDialog::status_text() const
    {
        return status_->text();
    }

    QString CodeFilterDialog::hint_text() const
    {
        return hint_->text();
    }

    QComboBox* CodeFilterDialog::module_combo() const noexcept
    {
        return module_combo_;
    }

    QCheckBox* CodeFilterDialog::call_checkbox() const noexcept
    {
        return call_;
    }

    QCheckBox* CodeFilterDialog::jump_checkbox() const noexcept
    {
        return jump_;
    }

    QCheckBox* CodeFilterDialog::conditional_checkbox() const noexcept
    {
        return conditional_;
    }

    QPushButton* CodeFilterDialog::collect_button() const noexcept
    {
        return collect_;
    }

    QPushButton* CodeFilterDialog::keep_executed_button() const noexcept
    {
        return keep_executed_;
    }

    QPushButton* CodeFilterDialog::keep_not_executed_button() const noexcept
    {
        return keep_not_executed_;
    }

    QPushButton* CodeFilterDialog::clear_button() const noexcept
    {
        return clear_;
    }

    QTableView* CodeFilterDialog::table() const noexcept
    {
        return table_;
    }

    int CodeFilterDialog::row_count() const
    {
        return model_->row_count();
    }

    std::uint64_t CodeFilterDialog::target_at(int row) const
    {
        return model_->target_at(row);
    }

    QString CodeFilterDialog::cell_text(int row, int column) const
    {
        const QModelIndex index = model_->index(row, column);
        return index.isValid() ? model_->data(index, Qt::DisplayRole).toString() : QString();
    }

}
