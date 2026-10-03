#include "ui/panels/found_list_panel.hpp"

#include <cstddef>
#include <utility>

#include <QAction>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

#include "ui/components/widgets.hpp"
#include "ui/models/found_results_model.hpp"

namespace slopkit::ui::panels
{

    namespace
    {
        // Compares the two snapshots' display data; the engine's snapshot is a
        // copy, so identity cannot be used.
        bool same_snapshot(const scan::ScanSnapshot& lhs, const scan::ScanSnapshot& rhs)
        {
            if (lhs.state != rhs.state || lhs.progress != rhs.progress || lhs.hit_count != rhs.hit_count
                || lhs.truncated != rhs.truncated || lhs.message != rhs.message || lhs.hits.size() != rhs.hits.size())
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

        header_ = widgets::section_header(tr("Found: 0"), this);
        layout->addWidget(header_);

        note_ = new widgets::StatusLabel(this);
        note_->setVisible(false);
        layout->addWidget(note_);

        model_      = new models::FoundResultsModel(this);
        table_view_ = new QTableView(this);
        table_view_->setModel(model_);
        table_view_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_view_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_view_->setAlternatingRowColors(true);
        table_view_->setSortingEnabled(true);
        table_view_->sortByColumn(models::FoundResultsModel::address, Qt::AscendingOrder);
        table_view_->verticalHeader()->setVisible(false);
        table_view_->horizontalHeader()->setStretchLastSection(true);
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
    }

    void FoundListPanel::set_target_attached(bool attached)
    {
        memory_view_button_->setEnabled(attached);
    }

    void FoundListPanel::refresh()
    {
        const scan::ScanSnapshot snapshot = engine_.snapshot();
        const scan::ScanConfig   config   = engine_.config();

        header_->setText(tr("Found: %1").arg(static_cast<qulonglong>(snapshot.hit_count)));

        if (snapshot.hits.size() < snapshot.hit_count)
        {
            QString note = tr("Showing the first %1 of %2 matches")
                               .arg(static_cast<qulonglong>(snapshot.hits.size()))
                               .arg(static_cast<qulonglong>(snapshot.hit_count));
            if (snapshot.truncated)
            {
                note += tr(" (result cap reached)");
            }
            note_->set_status(widgets::StatusKind::info, note);
        }
        else if (snapshot.truncated)
        {
            note_->set_status(widgets::StatusKind::warning,
                              tr("The result cap was reached; only the first matches are stored."));
        }
        else
        {
            note_->clear_status();
        }
        note_->setVisible(!note_->text().isEmpty());

        if (has_last_ && same_config(last_config_, config) && same_snapshot(last_snapshot_, snapshot))
        {
            return;
        }
        last_snapshot_ = snapshot;
        last_config_   = config;
        has_last_      = true;
        table_view_->clearSelection();
        model_->set_snapshot(std::move(snapshot), std::move(config));
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

    void FoundListPanel::show_context_menu(const QPoint& position)
    {
        const QModelIndex index = table_view_->indexAt(position);
        if (!index.isValid())
        {
            return;
        }

        QMenu    menu(this);
        QAction* add = menu.addAction(tr("Add to address table"));
        if (menu.exec(table_view_->viewport()->mapToGlobal(position)) == add)
        {
            add_to_table(index.row());
        }
    }

} // namespace slopkit::ui::panels
