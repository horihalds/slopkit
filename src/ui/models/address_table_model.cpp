#include "ui/models/address_table_model.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "scan/types.hpp"
#include "scan/value.hpp"
#include "ui/components/elided_tooltip_delegate.hpp"
#include "ui/components/indent_delegate.hpp"
#include "ui/fonts.hpp"
#include "ui/text.hpp"
#include "ui/theme.hpp"

#include <QByteArray>
#include <QMimeData>

namespace slopkit::ui::models
{
    namespace
    {
        constexpr char kRowMimeType[] = "application/x-slopkit-address-row";

        bool is_script(const table::AddressEntry& entry)
        {
            return entry.kind == table::EntryKind::script;
        }
    }

    AddressTableModel::AddressTableModel(table::AddressTable&     table,
                                         process::AccessWorker&   worker,
                                         process::AttachedTarget& target,
                                         QObject*                 parent)
        : QAbstractTableModel(parent), table_(table), worker_(worker), target_(target)
    {
    }

    int AddressTableModel::rowCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : static_cast<int>(table_.size());
    }

    int AddressTableModel::columnCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : column_count;
    }

    QVariant AddressTableModel::data(const QModelIndex& index, int role) const
    {
        const auto row = static_cast<std::size_t>(index.row());
        if (!index.isValid() || row >= table_.size())
        {
            return {};
        }

        const auto& entry = table_.entries()[row];

        switch (role)
        {
        case Qt::DisplayRole:
        case Qt::EditRole:
            switch (index.column())
            {
            case description:
                return to_qstring(entry.description);
            case address:
                if (is_script(entry))
                {
                    return {};
                }
                return address_text(entry.address);
            case type:
                return is_script(entry) ? tr("script") : to_qstring(scan::describe(entry.type));
            case value:
                if (is_script(entry))
                {
                    return {};
                }
                if (std::ranges::find(writing_entries_, entry.id) != writing_entries_.end())
                {
                    return tr("Writing...");
                }
                if (role == Qt::DisplayRole)
                {
                    const LiveCell* cell = live_cell_at(row);
                    if (cell != nullptr && cell->has_reading && !cell->readable)
                    {
                        return tr("?");
                    }
                    if (cell != nullptr && cell->has_reading && cell->bytes.has_value())
                    {
                        return to_qstring(scan::format_value(entry.type, *cell->bytes, entry.hex));
                    }
                }
                return to_qstring(table_.display_value(row));
            default:
                break;
            }
            break;
        case Qt::ForegroundRole:
            if (index.column() == value)
            {
                const LiveCell* cell = live_cell_at(row);
                if (cell != nullptr && cell->has_reading)
                {
                    if (!cell->readable)
                    {
                        return active_theme().text_muted;
                    }
                    if (cell->changed)
                    {
                        return active_theme().warning;
                    }
                }
            }
            break;
        case Qt::CheckStateRole:
            if (index.column() == active)
            {
                return entry.active ? Qt::Checked : Qt::Unchecked;
            }
            break;
        case Qt::FontRole:
            if (index.column() == address || index.column() == value)
            {
                return QVariant::fromValue(mono_font());
            }
            break;
        case Qt::TextAlignmentRole:
            if (index.column() == active)
            {
                return static_cast<int>(Qt::AlignCenter);
            }
            return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
        case Qt::ToolTipRole:
            if (index.column() == active)
            {
                return is_script(entry) ? tr("Runs activate() when ticked and deactivate() when unticked")
                                        : tr("Continuously write this value back to the target");
            }
            if (index.column() == address && !entry.expression.empty())
            {
                return to_qstring(entry.expression) + "\n" + ui::format_absolute(entry.address);
            }
            if (index.column() == value)
            {
                const LiveCell* cell = live_cell_at(row);
                if (cell != nullptr && cell->has_reading && !cell->readable)
                {
                    return tr("Not readable");
                }
            }
            break;
        case widgets::kFullTextRole:
            if (index.column() == address && !is_script(entry))
            {
                return ui::format_cell_address_full(address_mode_, module_spans_, entry.address);
            }
            break;
        case widgets::kDepthRole:
            if (index.column() == description)
            {
                return static_cast<int>(table_.depth_of(row));
            }
            break;
        default:
            break;
        }
        return {};
    }

    QVariant AddressTableModel::headerData(int section, Qt::Orientation orientation, int role) const
    {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole)
        {
            switch (section)
            {
            case description:
                return tr("Description");
            case address:
                return tr("Address");
            case type:
                return tr("Type");
            case value:
                return tr("Value");
            case active:
                return tr("Active");
            default:
                break;
            }
        }
        return QAbstractTableModel::headerData(section, orientation, role);
    }

    Qt::ItemFlags AddressTableModel::flags(const QModelIndex& index) const
    {
        if (!index.isValid())
        {
            return Qt::ItemIsDropEnabled;
        }

        Qt::ItemFlags item_flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
        if (const auto row = static_cast<std::size_t>(index.row());
            row < table_.size() && is_script(table_.entries()[row]))
        {
            if (index.column() == active)
            {
                item_flags |= Qt::ItemIsUserCheckable;
            }
            else if (index.column() == description)
            {
                item_flags |= Qt::ItemIsEditable;
            }
            return item_flags;
        }
        switch (index.column())
        {
        case description:
            item_flags |= Qt::ItemIsEditable;
            break;
        case active:
            item_flags |= Qt::ItemIsUserCheckable;
            break;
        default:
            break;
        }
        return item_flags;
    }

    Qt::DropActions AddressTableModel::supportedDropActions() const
    {
        return Qt::MoveAction;
    }

    QStringList AddressTableModel::mimeTypes() const
    {
        return {QString::fromLatin1(kRowMimeType)};
    }

    QMimeData* AddressTableModel::mimeData(const QModelIndexList& indexes) const
    {
        const int count        = rowCount();
        const int cursor       = table_.selected();
        int       lowest       = -1;
        bool      drags_cursor = false;
        for (const QModelIndex& index : indexes)
        {
            if (!index.isValid() || index.row() < 0 || index.row() >= count)
            {
                continue;
            }
            if (lowest < 0 || index.row() < lowest)
            {
                lowest = index.row();
            }
            if (index.row() == cursor)
            {
                drags_cursor = true;
            }
        }

        const int row = drags_cursor ? cursor : lowest;
        if (row < 0)
        {
            return nullptr;
        }

        auto* data = new QMimeData;
        data->setData(QString::fromLatin1(kRowMimeType), QByteArray::number(row));
        return data;
    }

    bool AddressTableModel::canDropMimeData(
        const QMimeData* data, Qt::DropAction action, int row, int, const QModelIndex& parent) const
    {
        if (data == nullptr || action != Qt::MoveAction || !data->hasFormat(QString::fromLatin1(kRowMimeType)))
        {
            return false;
        }
        bool      ok     = false;
        const int source = data->data(QString::fromLatin1(kRowMimeType)).toInt(&ok);
        const int count  = rowCount();
        if (!ok || source < 0 || source >= count)
        {
            return false;
        }

        const auto from = static_cast<std::size_t>(source);
        if (parent.isValid())
        {
            const int target = parent.row();
            if (target < 0 || target >= count)
            {
                return false;
            }
            return !table_.is_within_subtree(static_cast<std::size_t>(target), from);
        }

        const int insert = row < 0 ? count : std::clamp(row, 0, count);
        return !(insert > source && insert < source + static_cast<int>(table_.subtree_size(from)));
    }

    bool AddressTableModel::dropMimeData(
        const QMimeData* data, Qt::DropAction action, int row, int, const QModelIndex& parent)
    {
        if (data == nullptr || action != Qt::MoveAction || !data->hasFormat(QString::fromLatin1(kRowMimeType)))
        {
            return false;
        }

        bool      ok     = false;
        const int source = data->data(QString::fromLatin1(kRowMimeType)).toInt(&ok);
        if (!ok)
        {
            return false;
        }

        if (parent.isValid())
        {
            return drop_row(source, parent.row(), true);
        }
        return drop_row(source, row < 0 ? rowCount() : row, false);
    }

    bool AddressTableModel::drop_row(int source, int target, bool nest)
    {
        const int count = rowCount();
        if (source < 0 || source >= count)
        {
            return false;
        }
        if (nest ? (target < 0 || target >= count) : (target < 0 || target > count))
        {
            return false;
        }

        const auto from = static_cast<std::size_t>(source);
        if (target < count && table_.is_within_subtree(static_cast<std::size_t>(target), from))
        {
            return false;
        }

        const std::size_t landing = static_cast<std::size_t>(target);
        const std::size_t moved   = table_.subtree_size(from);
        std::size_t       before  = landing;
        std::uint64_t     parent  = 0;
        if (nest)
        {
            before = landing + table_.subtree_size(landing);
            parent = table_.entries()[landing].id;
        }
        else if (landing == static_cast<std::size_t>(count))
        {
            before = static_cast<std::size_t>(count);
            parent = table_.entries()[before - 1].parent;
        }
        else
        {
            parent = table_.entries()[landing].parent;
        }

        const std::size_t destination = from < before ? before - moved : before;
        if (destination == from && parent == table_.entries()[from].parent)
        {
            return false;
        }

        const auto moved_rows = static_cast<int>(moved);
        const auto final_row  = static_cast<int>(destination);

        const bool reorders = final_row != source;
        if (reorders)
        {
            const int destination_child = source < final_row ? final_row + moved_rows : final_row;
            if (!beginMoveRows(QModelIndex(), source, source + moved_rows - 1, QModelIndex(), destination_child))
            {
                return false;
            }
        }

        table_.move_row(from, landing, nest);

        const int first = std::min(source, final_row);
        const int last  = std::max(source, final_row) + moved_rows;
        for (int row = first; row < last && static_cast<std::size_t>(row) < cells_.size(); ++row)
        {
            cells_.cell(static_cast<std::size_t>(row)) = LiveCell {};
        }

        note_table_changed();
        if (reorders)
        {
            endMoveRows();
        }
        emit dataChanged(index(0, 0), index(rowCount() - 1, column_count - 1));
        emit statusChanged(nest ? tr("Row nested.") : tr("Row moved."), false);
        return true;
    }

    int AddressTableModel::depth_at(std::size_t row) const
    {
        return static_cast<int>(table_.depth_of(row));
    }

    bool AddressTableModel::setData(const QModelIndex& index, const QVariant& value, int role)
    {
        if (!index.isValid() || index.row() < 0)
        {
            return false;
        }
        const auto row = static_cast<std::size_t>(index.row());
        if (row >= table_.size())
        {
            return false;
        }

        if (is_script(table_.entries()[row]))
        {
            if (role == Qt::CheckStateRole && index.column() == active)
            {
                emit scriptActiveRequested(row, value.toInt() == Qt::Checked);
                emit dataChanged(index, index);
                return false;
            }
            if (role == Qt::EditRole && index.column() == description)
            {
                const QString name = value.toString().trimmed();
                if (name.isEmpty())
                {
                    emit statusChanged(tr("Description must not be empty."), true);
                    return false;
                }
                if (table_.set_description(row, name.toStdString()))
                {
                    note_table_changed();
                    emit dataChanged(index, index);
                    emit statusChanged(tr("Script renamed."), false);
                    return true;
                }
                return false;
            }
            return false;
        }

        if (role == Qt::CheckStateRole && index.column() == active)
        {
            table_.entries()[row].active = value.toInt() == Qt::Checked;
            note_table_changed();
            emit dataChanged(index, index);
            return true;
        }

        if (role != Qt::EditRole)
        {
            return false;
        }

        switch (index.column())
        {
        case Column::description:
            table_.entries()[row].description = value.toString().toStdString();
            note_table_changed();
            emit dataChanged(index, index);
            emit statusChanged(tr("Description updated."), false);
            return true;
        case Column::value:
            return write_value_at(index.row(), value.toString());
        default:
            return false;
        }
    }

    void AddressTableModel::refresh()
    {
        if (same_as_last())
        {
            return;
        }

        const auto& entries = table_.entries();
        if (last_entries_.size() != entries.size())
        {
            beginResetModel();
            cells_.reset(entries.size());
            note_table_changed();
            endResetModel();
            return;
        }

        for (std::size_t row = 0; row < entries.size(); ++row)
        {
            if (last_entries_[row].id != entries[row].id && row < cells_.size())
            {
                cells_.cell(row) = LiveCell {};
            }
        }

        note_table_changed();
        emit dataChanged(index(0, 0), index(rowCount() - 1, column_count - 1));
    }

    void AddressTableModel::set_modules(std::vector<process::ModuleInfo> modules)
    {
        ui::ModuleSpans spans;
        spans.set_modules(modules);
        if (spans == module_spans_)
        {
            return;
        }
        module_spans_ = std::move(spans);
        if (rowCount() > 0)
        {
            emit dataChanged(index(0, address), index(rowCount() - 1, address), {Qt::DisplayRole});
        }
    }

    void AddressTableModel::set_address_mode(ui::AddressMode mode)
    {
        if (address_mode_ == mode)
        {
            return;
        }
        address_mode_ = mode;
        if (rowCount() > 0)
        {
            emit dataChanged(index(0, address), index(rowCount() - 1, address), {Qt::DisplayRole});
        }
    }

    std::vector<LiveRequest> AddressTableModel::next_live_request()
    {
        const auto& entries = table_.entries();
        return cells_.request(entries.size(),
                              [this, &entries](std::size_t row)
                              {
                                  const auto& entry     = entries[row];
                                  const bool  value_row = entry.kind == table::EntryKind::value;
                                  const bool  readable =
                                      value_row && !entry.bytes.empty()
                                      && std::ranges::find(writing_entries_, entry.id) == writing_entries_.end();
                                  return LiveCandidate {.address  = entry.address,
                                                        .size     = value_row ? entry.bytes.size() : 0,
                                                        .identity = entry.id,
                                                        .readable = readable};
                              });
    }

    void AddressTableModel::apply_live_readings(std::span<const LiveReading> readings)
    {
        const auto& entries = table_.entries();
        cells_.apply(
            readings,
            [&entries](std::size_t row, std::uint64_t identity)
            {
                return row < entries.size() && entries[row].id == identity;
            },
            [this](std::size_t first, std::size_t last)
            {
                emit dataChanged(index(static_cast<int>(first), value),
                                 index(static_cast<int>(last), value),
                                 {Qt::DisplayRole, Qt::ForegroundRole});
            });
    }

    const LiveCell* AddressTableModel::live_cell_at(std::size_t row) const
    {
        return cells_.at(row);
    }

    void AddressTableModel::seed_live_write(std::uint64_t entry_id)
    {
        const auto& entries = table_.entries();
        for (std::size_t row = 0; row < entries.size() && row < cells_.size(); ++row)
        {
            if (entries[row].id != entry_id)
            {
                continue;
            }
            LiveCell& cell   = cells_.cell(row);
            cell.has_reading = true;
            cell.readable    = true;
            cell.bytes       = entries[row].bytes;
            cell.changed     = false;
            return;
        }
    }

    QString AddressTableModel::address_text(std::uint64_t address) const
    {
        return ui::format_cell_address(address_mode_, module_spans_, address);
    }

    bool AddressTableModel::write_value_at(int row, const QString& text)
    {
        if (row < 0)
        {
            return false;
        }
        const std::size_t index_row = static_cast<std::size_t>(row);
        return write_values(std::span<const std::size_t>(&index_row, 1), text);
    }

    bool AddressTableModel::rows_share_value_format(std::span<const std::size_t> rows) const
    {
        if (rows.empty() || !table_.valid_index(rows.front()))
        {
            return false;
        }
        const auto& first = table_.entries()[rows.front()];
        if (first.kind != table::EntryKind::value)
        {
            return false;
        }
        return std::ranges::all_of(rows,
                                   [this, &first](std::size_t row)
                                   {
                                       if (!table_.valid_index(row))
                                       {
                                           return false;
                                       }
                                       const auto& entry = table_.entries()[row];
                                       return entry.kind == table::EntryKind::value && entry.type == first.type
                                           && entry.hex == first.hex;
                                   });
    }

    bool AddressTableModel::write_values(std::span<const std::size_t> rows, const QString& text)
    {
        if (rows.empty())
        {
            return false;
        }
        if (!target_.valid())
        {
            emit statusChanged(tr("Not attached; cannot write."), true);
            return false;
        }
        if (write_pending_.has_value())
        {
            emit statusChanged(tr("A write is already in progress."), true);
            return false;
        }
        if (!rows_share_value_format(rows))
        {
            log::warning(log::category::ui, "table write rejected: the selection mixes types or formats");
            emit statusChanged(tr("Select entries of the same type and format."), true);
            return false;
        }

        const std::size_t first_row = rows.front();
        const auto&       first     = table_.entries()[first_row];
        const std::string input     = text.toStdString();
        if (const auto parsed = scan::parse_value(first.type, input, first.hex); !parsed)
        {
            log::warning(log::category::ui, std::format("table value rejected: {}", parsed.error().message));
            emit statusChanged(tr("Value: %1").arg(to_qstring(parsed.error().message)), true);
            return false;
        }
        auto encoded = table_.encode_value(first_row, input);
        if (!encoded)
        {
            log::warning(log::category::ui, std::format("table value rejected: {}", input));
            emit statusChanged(tr("Value: invalid value."), true);
            return false;
        }

        std::vector<process::WriteItem> items;
        std::vector<std::uint64_t>      entries;
        items.reserve(rows.size());
        entries.reserve(rows.size());
        for (const std::size_t row : rows)
        {
            const auto& entry = table_.entries()[row];
            items.push_back(process::WriteItem {.id = entry.id, .address = entry.address, .bytes = *encoded});
            entries.push_back(entry.id);
        }

        const process::JobId job_id = worker_.next_job_id();
        write_pending_              = job_id;
        writing_entries_            = entries;

        const bool submitted = worker_.submit_freeze(
            job_id,
            std::move(items),
            [this, job_id, entries, cached = *encoded](process::JobResult&& result)
            {
                if (write_pending_ != job_id)
                {
                    return;
                }
                write_pending_.reset();
                writing_entries_.clear();

                const auto& freeze = std::get<process::FreezeResult>(result);
                for (std::size_t index = 0; index < freeze.written && index < entries.size(); ++index)
                {
                    table_.apply_write(entries[index], cached);
                    seed_live_write(entries[index]);
                }
                note_table_changed();
                emit dataChanged(index(0, 0), index(rowCount() - 1, column_count - 1));
                if (freeze.error)
                {
                    log::warning(log::category::ui,
                                 std::format("table write failed: {}", process::describe(*freeze.error)));
                    emit statusChanged(tr("Write failed: %1").arg(to_qstring(process::describe(*freeze.error))), true);
                    return;
                }
                if (entries.size() == 1)
                {
                    emit statusChanged(tr("Value written."), false);
                }
                else
                {
                    emit statusChanged(tr("%1 values written.").arg(static_cast<int>(entries.size())), false);
                }
            });
        if (!submitted)
        {
            write_pending_.reset();
            writing_entries_.clear();
            emit statusChanged(tr("Write unavailable."), true);
            return false;
        }

        emit dataChanged(index(static_cast<int>(rows.front()), value), index(static_cast<int>(rows.back()), value));
        return true;
    }

    bool AddressTableModel::same_as_last() const
    {
        const auto entries = table_.entries();
        if (entries.size() != last_entries_.size())
        {
            return false;
        }
        for (std::size_t index = 0; index < entries.size(); ++index)
        {
            const auto& current = entries[index];
            const auto& last    = last_entries_[index];
            if (current.id != last.id || current.parent != last.parent || current.active != last.active
                || current.kind != last.kind || current.description != last.description
                || current.address != last.address || current.expression != last.expression || current.type != last.type
                || current.hex != last.hex || current.bytes != last.bytes || current.script != last.script)
            {
                return false;
            }
        }
        return true;
    }

    void AddressTableModel::note_table_changed()
    {
        const auto entries = table_.entries();
        last_entries_.assign(entries.begin(), entries.end());
    }

    void AddressTableModel::note_entry_changed(std::size_t row)
    {
        note_table_changed();
        if (row < static_cast<std::size_t>(rowCount()))
        {
            const int first = static_cast<int>(row);
            emit      dataChanged(index(first, 0), index(first, column_count - 1));
        }
    }

}
