#include "ui/panels/address_list_panel.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <QAction>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

#include "table/serializer.hpp"
#include "ui/components/widgets.hpp"
#include "ui/models/address_table_model.hpp"

namespace slopkit::ui::panels
{

    namespace
    {
        QString to_qstring(std::string_view text)
        {
            return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
        }

        // A menu entry that stays visible but explains why it is unavailable.
        QAction* disabled_action(QMenu& menu, const QString& text, const QString& reason)
        {
            QAction* action = menu.addAction(text);
            action->setEnabled(false);
            action->setToolTip(reason);
            return action;
        }
    } // namespace

    AddressListPanel::AddressListPanel(table::AddressTable&     table,
                                       process::AccessWorker&   worker,
                                       process::AttachedTarget& target,
                                       QWidget*                 parent)
        : QWidget(parent), table_(table)
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(6);

        model_      = new models::AddressTableModel(table_, worker, target, this);
        table_view_ = new QTableView(this);
        table_view_->setModel(model_);
        table_view_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_view_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_view_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
        table_view_->setAlternatingRowColors(true);
        table_view_->verticalHeader()->setVisible(false);
        table_view_->horizontalHeader()->setSectionResizeMode(models::AddressTableModel::description,
                                                              QHeaderView::Stretch);
        table_view_->horizontalHeader()->setSectionResizeMode(models::AddressTableModel::address,
                                                              QHeaderView::ResizeToContents);
        table_view_->horizontalHeader()->setSectionResizeMode(models::AddressTableModel::type,
                                                              QHeaderView::ResizeToContents);
        table_view_->horizontalHeader()->setSectionResizeMode(models::AddressTableModel::value, QHeaderView::Stretch);
        table_view_->horizontalHeader()->setSectionResizeMode(models::AddressTableModel::frozen,
                                                              QHeaderView::ResizeToContents);
        layout->addWidget(table_view_, 1);

        connect(table_view_->selectionModel(),
                &QItemSelectionModel::currentRowChanged,
                this,
                [this](const QModelIndex& current)
                {
                    table_.set_selected(current.isValid() ? current.row() : -1);
                    refresh();
                });

        connect(model_,
                &models::AddressTableModel::statusChanged,
                this,
                [this](const QString& message, bool is_error)
                {
                    set_status(message, is_error);
                });

        table_view_->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(table_view_,
                &QWidget::customContextMenuRequested,
                this,
                [this](const QPoint& position)
                {
                    show_context_menu(position);
                });

        // The address-table actions sit at the far right of the footer, so the
        // Add Address button lands against the window's right edge.
        auto* footer        = new QHBoxLayout();
        add_address_button_ = widgets::secondary_button(tr("Add Address Manually"), this);
        footer->addStretch(1);
        footer->addWidget(add_address_button_);
        layout->addLayout(footer);
        connect(add_address_button_, &QPushButton::clicked, this, &AddressListPanel::addAddressRequested);

        status_label_ = new widgets::StatusLabel(this);
        layout->addWidget(status_label_);

        refresh();
    }

    void AddressListPanel::set_status(const QString& message, bool is_error)
    {
        status_          = message;
        status_is_error_ = is_error;
        refresh();
    }

    void AddressListPanel::refresh()
    {
        model_->refresh();

        if (!status_.isEmpty())
        {
            status_label_->set_status(status_is_error_ ? widgets::StatusKind::error : widgets::StatusKind::info,
                                      status_);
        }
        else
        {
            status_label_->clear_status();
        }
    }

    void AddressListPanel::open_table()
    {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Open Table"), table_path_, tr("Address tables (*.txt);;All files (*)"));
        if (path.isEmpty())
        {
            return;
        }
        table_path_ = path;

        if (const auto result = table::load(std::filesystem::path(path.toStdString()), table_); !result)
        {
            set_status(tr("Open failed: %1").arg(to_qstring(result.error())), true);
            return;
        }
        set_status(tr("Loaded %1 entries.").arg(static_cast<qulonglong>(table_.size())), false);
    }

    void AddressListPanel::save_table()
    {
        if (table_path_.isEmpty())
        {
            save_table_as();
            return;
        }
        save_to_path(table_path_);
    }

    void AddressListPanel::save_table_as()
    {
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Save Table As"), table_path_, tr("Address tables (*.txt);;All files (*)"));
        if (path.isEmpty())
        {
            return;
        }
        table_path_ = path;
        save_to_path(path);
    }

    void AddressListPanel::save_to_path(const QString& path)
    {
        if (const auto result = table::save(std::filesystem::path(path.toStdString()), table_); !result)
        {
            set_status(tr("Save failed: %1").arg(to_qstring(result.error())), true);
            return;
        }
        set_status(tr("Saved %1 entries.").arg(static_cast<qulonglong>(table_.size())), false);
    }

    void AddressListPanel::delete_selected()
    {
        const int row = table_.selected();
        if (row < 0)
        {
            set_status(tr("Select an entry to delete."), true);
            return;
        }

        const auto& entry   = table_.entries()[static_cast<std::size_t>(row)];
        const auto  address = QStringLiteral("0x") + QString::number(entry.address, 16).toUpper();
        const auto  label   = entry.description.empty() ? address : to_qstring(entry.description);

        const auto answer = QMessageBox::question(
            this, tr("Delete Entry"), tr("Delete %1?").arg(label), QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
        {
            return;
        }

        table_.remove(static_cast<std::size_t>(row));
        set_status(tr("Entry deleted."), false);
    }

    void AddressListPanel::toggle_freeze_selected()
    {
        const int row = table_.selected();
        if (row < 0)
        {
            set_status(tr("Select an entry to freeze."), true);
            return;
        }

        auto& entry  = table_.entries()[static_cast<std::size_t>(row)];
        entry.active = !entry.active;
        set_status(entry.active ? tr("Entry frozen.") : tr("Entry unfrozen."), false);
    }

    void AddressListPanel::report_freeze_error(std::string_view message)
    {
        set_status(tr("Freeze failed: %1").arg(to_qstring(message)), true);
    }

    void AddressListPanel::show_context_menu(const QPoint& position)
    {
        const QModelIndex index = table_view_->indexAt(position);
        if (!index.isValid())
        {
            return;
        }

        const auto row = static_cast<std::size_t>(index.row());
        if (!table_.valid_index(row))
        {
            return;
        }
        table_.set_selected(index.row());

        auto& entry = table_.entries()[row];

        QMenu menu(this);
        menu.setToolTipsVisible(true);

        QAction* change_value = menu.addAction(tr("Change value"));
        QAction* freeze       = menu.addAction(tr("Freeze"));
        freeze->setCheckable(true);
        freeze->setChecked(entry.active);
        QAction* show_hex = menu.addAction(tr("Show as hex"));
        show_hex->setCheckable(true);
        show_hex->setChecked(entry.hex);
        QAction* browse = menu.addAction(tr("Browse this memory region"));

        menu.addSeparator();
        disabled_action(
            menu, tr("Find out what writes to this address"), tr("Disabled: needs hardware watchpoints or ptrace"));
        disabled_action(menu, tr("Group"), tr("Disabled: address groups are not implemented"));

        menu.addSeparator();
        QAction* remove = menu.addAction(tr("Delete"));

        QAction* chosen = menu.exec(table_view_->viewport()->mapToGlobal(position));

        if (chosen == change_value)
        {
            table_view_->edit(model_->index(index.row(), models::AddressTableModel::value));
        }
        else if (chosen == freeze)
        {
            entry.active = freeze->isChecked();
            set_status(entry.active ? tr("Entry frozen.") : tr("Entry unfrozen."), false);
        }
        else if (chosen == show_hex)
        {
            entry.hex = show_hex->isChecked();
            set_status(tr("Display format updated."), false);
        }
        else if (chosen == browse)
        {
            emit browseRequested(entry.address);
        }
        else if (chosen == remove)
        {
            delete_selected();
        }
    }

} // namespace slopkit::ui::panels
