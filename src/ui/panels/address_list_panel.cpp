#include "ui/panels/address_list_panel.hpp"

#include <cstddef>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include <QAction>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QMenu>
#include <QTableView>
#include <QVBoxLayout>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "table/serializer.hpp"
#include "ui/components/message_box.hpp"
#include "ui/models/address_table_model.hpp"
#include "ui/table_file.hpp"

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

        refresh();
    }

    QWidget* AddressListPanel::tab_order_first() const noexcept
    {
        return table_view_;
    }

    void AddressListPanel::set_status(const QString& message, bool is_error)
    {
        if (message == status_ && is_error == status_is_error_)
        {
            return;
        }
        status_          = message;
        status_is_error_ = is_error;
        emit statusChanged(message, is_error);
    }

    void AddressListPanel::refresh()
    {
        model_->refresh();
    }

    void AddressListPanel::set_modules(std::vector<process::ModuleInfo> modules)
    {
        model_->set_modules(std::move(modules));
    }

    void AddressListPanel::set_address_mode(ui::AddressMode mode)
    {
        model_->set_address_mode(mode);
    }

    void AddressListPanel::set_table_path(const QString& path)
    {
        if (table_path_ == path)
        {
            return;
        }
        table_path_ = path;
        emit tablePathChanged(path);
    }

    QString AddressListPanel::table_path() const
    {
        return table_path_;
    }

    void AddressListPanel::set_dialog_directory(const QString& directory)
    {
        dialog_directory_ = directory;
    }

    std::optional<QString> AddressListPanel::choose_table_path()
    {
        log::debug(log::category::ui, "open table requested");
        const QString start = dialog_directory_.isEmpty() ? table_path_ : dialog_directory_;
        const QString path =
            QFileDialog::getOpenFileName(this, tr("Open Table"), start, tr("Address tables (*.skt);;All files (*)"));
        if (path.isEmpty())
        {
            return std::nullopt;
        }
        return path;
    }

    std::optional<table::AddressTable> AddressListPanel::parse_table(const QString& path)
    {
        table::AddressTable parsed;
        if (const auto result = table::load(std::filesystem::path(path.toStdString()), parsed); !result)
        {
            log::warning(log::category::ui, std::format("table load failed: {}", result.error()));
            set_status(tr("Open failed: %1").arg(to_qstring(result.error())), true);
            return std::nullopt;
        }
        return parsed;
    }

    void AddressListPanel::adopt_table(const QString& path, table::AddressTable&& loaded)
    {
        set_table_path(path);
        table_ = std::move(loaded);
        model_->refresh();
        set_status(tr("Loaded %1 entries.").arg(static_cast<qulonglong>(table_.size())), false);
        emit tableLoaded();
    }

    table::MergeSummary AddressListPanel::merge_table(const table::AddressTable& incoming)
    {
        const auto summary = table_.merge(incoming.entries());
        model_->refresh();
        set_status(tr("Merged: %1 added, %2 skipped.")
                       .arg(static_cast<qulonglong>(summary.added))
                       .arg(static_cast<qulonglong>(summary.skipped)),
                   false);
        return summary;
    }

    bool AddressListPanel::load_table(const QString& path)
    {
        auto parsed = parse_table(path);
        if (!parsed)
        {
            // The path is still remembered, so a later save keeps the target.
            set_table_path(path);
            return false;
        }
        adopt_table(path, std::move(*parsed));
        return true;
    }

    void AddressListPanel::save_table()
    {
        log::debug(log::category::ui, "save table requested");
        if (table_path_.isEmpty())
        {
            save_table_as();
            return;
        }
        save_to_path(table_path_);
    }

    void AddressListPanel::save_table_as()
    {
        log::debug(log::category::ui, "save table as requested");
        QString suggested = table_path_.isEmpty() ? default_table_path() : table_path_;
        if (!dialog_directory_.isEmpty())
        {
            suggested = QDir(dialog_directory_).filePath(QFileInfo(suggested).fileName());
        }
        const QString chosen = QFileDialog::getSaveFileName(
            this, tr("Save Table As"), suggested, tr("Address tables (*.skt);;All files (*)"));
        if (chosen.isEmpty())
        {
            return;
        }
        save_to_path(table_file_path(chosen));
    }

    void AddressListPanel::save_to_path(const QString& path)
    {
        set_table_path(path);
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
            log::warning(log::category::ui, "delete requested with no selection");
            set_status(tr("Select an entry to delete."), true);
            return;
        }

        const auto& entry   = table_.entries()[static_cast<std::size_t>(row)];
        const auto  address = model_->address_text(entry.address);
        const auto  label   = entry.description.empty() ? address : to_qstring(entry.description);

        if (!widgets::confirm(this, tr("Delete Entry"), tr("Delete %1?").arg(label)))
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
            log::warning(log::category::ui, "freeze requested with no selection");
            set_status(tr("Select an entry to freeze."), true);
            return;
        }

        auto& entry  = table_.entries()[static_cast<std::size_t>(row)];
        entry.active = !entry.active;
        log::info(log::category::ui, entry.active ? "entry frozen" : "entry unfrozen");
        set_status(entry.active ? tr("Entry frozen.") : tr("Entry unfrozen."), false);
    }

    void AddressListPanel::report_status(std::string_view message, bool is_error)
    {
        set_status(to_qstring(message), is_error);
    }

    void AddressListPanel::report_freeze_error(std::string_view message)
    {
        report_status(std::format("Freeze failed: {}", message), true);
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
