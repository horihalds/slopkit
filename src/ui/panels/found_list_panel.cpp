#include "ui/panels/found_list_panel.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include <QAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

#include "ui/components/elided_tooltip_delegate.hpp"
#include "ui/components/widgets.hpp"
#include "ui/models/found_results_model.hpp"

namespace slopkit::ui::panels
{

    namespace
    {
        // Compares the two snapshots' display data. A finished scan shares its
        // whole result set with the engine, so its handle doubles as the change
        // signal; anything else (an idle or failed snapshot) is compared through
        // its display page.
        bool same_snapshot(const scan::ScanSnapshot& lhs, const scan::ScanSnapshot& rhs)
        {
            if (lhs.state != rhs.state || lhs.progress != rhs.progress || lhs.hit_count != rhs.hit_count
                || lhs.truncated != rhs.truncated || lhs.message != rhs.message)
            {
                return false;
            }
            if (lhs.result_hits.get() != rhs.result_hits.get())
            {
                return false;
            }
            if (lhs.result_hits)
            {
                return true;
            }
            if (lhs.hits.size() != rhs.hits.size())
            {
                return false;
            }
            for (std::size_t index = 0; index < lhs.hits.size(); ++index)
            {
                if (lhs.hits[index].address != rhs.hits[index].address || lhs.hits[index].value != rhs.hits[index].value
                    || lhs.hits[index].previous != rhs.hits[index].previous)
                {
                    return false;
                }
            }
            return true;
        }

        bool same_config(const scan::ScanConfig& lhs, const scan::ScanConfig& rhs)
        {
            return lhs.value_type == rhs.value_type && lhs.hex == rhs.hex;
        }
    } // namespace

    FoundListPanel::FoundListPanel(scan::ScanEngine& engine, table::AddressTable& table, QWidget* parent)
        : QWidget(parent), engine_(engine), table_(table)
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(6);

        header_ = widgets::section_header(tr("Showing 0 of 0 results"), this);
        layout->addWidget(header_);

        model_      = new models::FoundResultsModel(this);
        table_view_ = new QTableView(this);
        table_view_->setModel(model_);
        table_view_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_view_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_view_->setAlternatingRowColors(true);
        table_view_->setSortingEnabled(true);
        table_view_->setItemDelegate(new widgets::ElidedTooltipDelegate(table_view_));
        table_view_->sortByColumn(models::FoundResultsModel::address, Qt::AscendingOrder);
        table_view_->verticalHeader()->setVisible(false);
        table_view_->horizontalHeader()->setStretchLastSection(true);
        // The table takes the whole scan zone so its bottom edge stays level
        // with the Memory Scan Options panel's; no spacer may sit between it
        // and the entry row below.
        layout->addWidget(table_view_, 1);

        connect(table_view_,
                &QTableView::doubleClicked,
                this,
                [this](const QModelIndex& index)
                {
                    add_to_table(index.row());
                });

        table_view_->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(table_view_,
                &QWidget::customContextMenuRequested,
                this,
                [this](const QPoint& position)
                {
                    show_context_menu(position);
                });

        auto* entry_row     = new QHBoxLayout();
        memory_view_button_ = widgets::secondary_button(tr("Memory View"), this);
        memory_view_button_->setToolTip(tr("Open the Memory Viewer at the main module's entry point (or its base)"));
        memory_view_button_->setEnabled(false);
        entry_row->addWidget(memory_view_button_);
        entry_row->addStretch(1);
        layout->addLayout(entry_row);

        connect(memory_view_button_, &QPushButton::clicked, this, &FoundListPanel::memoryViewRequested);

        widgets::chain_tab_order({table_view_, memory_view_button_});
    }

    void FoundListPanel::set_target_attached(bool attached)
    {
        memory_view_button_->setEnabled(attached);
    }

    void FoundListPanel::set_modules(std::vector<process::ModuleInfo> modules)
    {
        model_->set_modules(std::move(modules));
    }

    void FoundListPanel::set_address_mode(ui::AddressMode mode)
    {
        model_->set_address_mode(mode);
    }

    std::vector<ui::LiveRequest> FoundListPanel::next_live_request()
    {
        return model_->next_live_request();
    }

    void FoundListPanel::apply_live_readings(std::span<const ui::LiveReading> readings)
    {
        model_->apply_live_readings(readings);
    }

    QWidget* FoundListPanel::tab_order_first() const noexcept
    {
        return table_view_;
    }

    QWidget* FoundListPanel::tab_order_last() const noexcept
    {
        return memory_view_button_;
    }

    void FoundListPanel::refresh()
    {
        const scan::ScanSnapshot snapshot = engine_.snapshot();
        const scan::ScanConfig   config   = engine_.config();
        const bool               running  = snapshot.state == scan::ScanState::running;

        if (running)
        {
            // A running scan shows no rows; they appear with the finished set.
            if (!has_last_ || last_snapshot_.state != scan::ScanState::running)
            {
                table_view_->clearSelection();
                model_->clear();
            }
        }
        else if (!has_last_ || !same_config(last_config_, config) || !same_snapshot(last_snapshot_, snapshot))
        {
            table_view_->clearSelection();
            model_->set_snapshot(snapshot, config);
        }

        last_snapshot_ = snapshot;
        last_config_   = config;
        has_last_      = true;

        if (running)
        {
            const int percent = static_cast<int>(std::clamp(snapshot.progress, 0.0f, 1.0f) * 100.0f);
            header_->setText(tr("Scanning... %1%").arg(percent));
            return;
        }

        QString line = tr("Showing %1 of %2 results")
                           .arg(static_cast<qulonglong>(model_->rowCount()))
                           .arg(static_cast<qulonglong>(last_snapshot_.hit_count));
        if (last_snapshot_.truncated)
        {
            line += tr(" (result cap reached)");
        }
        header_->setText(line);
    }

    void FoundListPanel::add_to_table(int row)
    {
        const scan::ScanHit* hit = model_->hit_at(row);
        if (hit == nullptr)
        {
            return;
        }

        const scan::ScanConfig config = engine_.config();
        table::AddressEntry    entry;
        entry.address = hit->address;
        entry.type    = config.value_type;
        entry.bytes   = hit->value;
        entry.hex     = config.hex;
        table_.add(std::move(entry));
    }

    void FoundListPanel::populate_row_menu(QMenu& menu, int row)
    {
        QAction* add = menu.addAction(tr("Add to address table"));
        connect(add,
                &QAction::triggered,
                this,
                [this, row]
                {
                    add_to_table(row);
                });

        QMenu* copy = menu.addMenu(tr("Copy"));
        connect(copy->addAction(tr("Address (module + RVA)")),
                &QAction::triggered,
                this,
                [this, row]
                {
                    copy_row(row, models::CopyFormat::module_relative);
                });
        connect(copy->addAction(tr("Address (absolute)")),
                &QAction::triggered,
                this,
                [this, row]
                {
                    copy_row(row, models::CopyFormat::absolute);
                });
        connect(copy->addAction(tr("Address + value")),
                &QAction::triggered,
                this,
                [this, row]
                {
                    copy_row(row, models::CopyFormat::address_and_value);
                });
    }

    void FoundListPanel::copy_row(int row, models::CopyFormat format)
    {
        const QString text = model_->copy_text(row, format);
        if (!text.isEmpty())
        {
            QGuiApplication::clipboard()->setText(text);
        }
    }

    void FoundListPanel::show_context_menu(const QPoint& position)
    {
        const QModelIndex index = table_view_->indexAt(position);
        if (!index.isValid())
        {
            return;
        }

        QMenu menu(this);
        populate_row_menu(menu, index.row());
        menu.exec(table_view_->viewport()->mapToGlobal(position));
    }

} // namespace slopkit::ui::panels
