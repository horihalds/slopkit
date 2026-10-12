#include "ui/panels/found_list_panel.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include <QAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

#include "scan/types.hpp"
#include "scan/value.hpp"
#include "ui/components/elided_tooltip_delegate.hpp"
#include "ui/components/input_box.hpp"
#include "ui/components/row_menu.hpp"
#include "ui/components/widgets.hpp"
#include "ui/models/found_results_model.hpp"

namespace slopkit::ui::panels
{
    namespace
    {
        bool same_page(const scan::ScanResults& lhs, const scan::ScanResults& rhs)
        {
            if (lhs.hits.size() != rhs.hits.size() || lhs.keeps_previous != rhs.keeps_previous)
            {
                return false;
            }
            for (std::size_t index = 0; index < lhs.hits.size(); ++index)
            {
                const scan::ScanHit& left  = lhs.hits[index];
                const scan::ScanHit& right = rhs.hits[index];
                if (left.address != right.address || !std::ranges::equal(lhs.value(left), rhs.value(right))
                    || !std::ranges::equal(lhs.previous(left), rhs.previous(right)))
                {
                    return false;
                }
            }
            return true;
        }

        bool same_snapshot(const scan::ScanSnapshot& lhs, const scan::ScanSnapshot& rhs)
        {
            if (lhs.state != rhs.state || lhs.progress != rhs.progress || lhs.hit_count != rhs.hit_count
                || lhs.message != rhs.message)
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
            return same_page(lhs.page, rhs.page);
        }

        bool same_config(const scan::ScanConfig& lhs, const scan::ScanConfig& rhs)
        {
            return lhs.value_type == rhs.value_type && lhs.hex == rhs.hex;
        }
    }

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
        table_view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table_view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_view_->setAlternatingRowColors(true);
        table_view_->setSortingEnabled(true);
        table_view_->setItemDelegate(new widgets::ElidedTooltipDelegate(table_view_));
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

        edit_value_action_ = new QAction(tr("Change value…"), this);
        edit_value_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+E")));
        edit_value_action_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        connect(edit_value_action_, &QAction::triggered, this, &FoundListPanel::edit_selected_value);
        table_view_->addAction(edit_value_action_);

        browse_region_action_ = new QAction(tr("Browse this memory region"), this);
        browse_region_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+D")));
        browse_region_action_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        connect(browse_region_action_, &QAction::triggered, this, &FoundListPanel::browse_selected_region);
        table_view_->addAction(browse_region_action_);

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
        target_attached_ = attached;
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

        header_->setText(tr("Showing %1 of %2 results")
                             .arg(static_cast<qulonglong>(model_->rowCount()))
                             .arg(static_cast<qulonglong>(last_snapshot_.hit_count)));
    }

    void FoundListPanel::add_to_table(int row)
    {
        const scan::ScanHit* hit = model_->hit_at(row);
        if (hit == nullptr)
        {
            return;
        }

        const std::span<const std::byte> value  = model_->value_at(row);
        const scan::ScanConfig           config = engine_.config();
        table::AddressEntry              entry;
        entry.address = hit->address;
        entry.type    = config.value_type;
        entry.bytes.assign(value.begin(), value.end());
        entry.hex = config.hex;
        table_.add(std::move(entry));
    }

    void FoundListPanel::edit_value(int row)
    {
        const scan::ScanHit* hit = model_->hit_at(row);
        if (hit == nullptr)
        {
            return;
        }

        std::vector<std::uint64_t> addresses;
        const QItemSelectionModel* selection = table_view_->selectionModel();
        if (selection != nullptr && selection->isRowSelected(row, QModelIndex()))
        {
            for (const QModelIndex& index : selection->selectedRows())
            {
                if (const scan::ScanHit* selected = model_->hit_at(index.row()); selected != nullptr)
                {
                    addresses.push_back(selected->address);
                }
            }
        }
        if (addresses.empty())
        {
            addresses.push_back(hit->address);
        }

        const scan::ScanConfig config = engine_.config();
        const QString          current =
            model_->data(model_->index(row, models::FoundResultsModel::value), Qt::DisplayRole).toString();

        widgets::InputBoxOptions options;
        options.title     = tr("Change value");
        options.label     = tr("Value:");
        options.initial   = current;
        options.monospace = true;
        options.validate  = [config](const QString& text)
        {
            const auto parsed = scan::parse_value(config.value_type, text.toStdString(), config.hex);
            return parsed ? QString() : QString::fromStdString(parsed.error().message);
        };

        const std::optional<QString> typed = widgets::get_text(options, this);
        if (!typed.has_value())
        {
            return;
        }
        const auto parsed = scan::parse_value(config.value_type, typed->toStdString(), config.hex);
        if (!parsed.has_value())
        {
            return;
        }
        emit changeValuesRequested(std::move(addresses), scan::encode_value(config.value_type, *parsed));
    }

    void FoundListPanel::browse_region(int row)
    {
        if (const scan::ScanHit* hit = model_->hit_at(row); hit != nullptr)
        {
            emit browseRequested(hit->address);
        }
    }

    void FoundListPanel::edit_selected_value()
    {
        const QModelIndex index = table_view_->currentIndex();
        if (index.isValid())
        {
            edit_value(index.row());
        }
    }

    void FoundListPanel::browse_selected_region()
    {
        const QModelIndex index = table_view_->currentIndex();
        if (index.isValid())
        {
            browse_region(index.row());
        }
    }

    void FoundListPanel::populate_row_menu(QMenu& menu, int row)
    {
        widgets::show_explanations(menu);

        QAction* add = menu.addAction(tr("Add to address table"));
        connect(add,
                &QAction::triggered,
                this,
                [this, row]
                {
                    add_to_table(row);
                });

        if (target_attached_)
        {
            QAction* edit = widgets::described_action(
                menu, tr("Change value…"), tr("Writes the typed value to every selected hit's address."));
            widgets::show_shortcut_hint(*edit, *edit_value_action_);
            connect(edit,
                    &QAction::triggered,
                    this,
                    [this, row]
                    {
                        edit_value(row);
                    });
        }
        else
        {
            QAction* edit = widgets::disabled_action(menu, tr("Change value…"), tr("Attach to a target first."));
            widgets::show_shortcut_hint(*edit, *edit_value_action_);
        }

        QMenu* copy = widgets::explained_submenu(menu, tr("Copy"));
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

        QAction* browse = widgets::described_action(
            menu, tr("Browse this memory region"), tr("Shows this hit's address in the Memory Viewer."));
        widgets::show_shortcut_hint(*browse, *browse_region_action_);
        connect(browse,
                &QAction::triggered,
                this,
                [this, row]
                {
                    browse_region(row);
                });

        QAction* dissect =
            target_attached_
                ? widgets::described_action(
                      menu, tr("Dissect as struct…"), tr("Opens a struct view at this hit's address."))
                : widgets::disabled_action(menu, tr("Dissect as struct…"), tr("Attach to a target first."));
        connect(dissect,
                &QAction::triggered,
                this,
                [this, row]
                {
                    const scan::ScanHit* hit = model_->hit_at(row);
                    if (hit != nullptr)
                    {
                        emit dissectRequested(hit->address);
                    }
                });

        const std::size_t width = scan::value_size(engine_.config().value_type);
        widgets::add_watch_commands(menu,
                                    target_attached_ && model_->hit_at(row) != nullptr,
                                    [this, row, width](widgets::WatchCommand command)
                                    {
                                        const scan::ScanHit* hit = model_->hit_at(row);
                                        if (hit == nullptr)
                                        {
                                            return;
                                        }
                                        const debug::Kind kind = command == widgets::WatchCommand::writes
                                                                   ? debug::Kind::hardware_write
                                                                   : debug::Kind::hardware_read_write;
                                        emit              accessWatchRequested(hit->address, width, kind);
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

        if (QItemSelectionModel* selection = table_view_->selectionModel();
            selection != nullptr && !selection->isRowSelected(index.row(), QModelIndex()))
        {
            selection->select(QItemSelection(index, index),
                              QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            table_view_->setCurrentIndex(index);
        }

        QMenu menu(this);
        populate_row_menu(menu, index.row());
        menu.exec(table_view_->viewport()->mapToGlobal(position));
    }

}
