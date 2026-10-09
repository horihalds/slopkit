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
#include "ui/fonts.hpp"
#include "ui/text.hpp"
#include "ui/theme.hpp"

#include <QByteArray>
#include <QMimeData>

namespace slopkit::ui::models
{

    namespace
    {
        // The move-only drag payload: the source row, carried as its index.
        constexpr char kRowMimeType[] = "application/x-slopkit-address-row";

        // A script row carries no address and no value: the row is read-only in
        // the grid and is run through its own row-menu action.
        bool is_script(const table::AddressEntry& entry)
        {
            return entry.kind == table::EntryKind::script;
        }
    } // namespace

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
                    return {}; // A script has no address.
                }
                if (!entry.expression.empty())
                {
                    return to_qstring(entry.expression);
                }
                return address_text(entry.address);
            case type:
                return is_script(entry) ? tr("script") : to_qstring(scan::describe(entry.type));
            case value:
                if (is_script(entry))
                {
                    return {}; // A script has no value.
                }
                if (writing_entry_.has_value() && *writing_entry_ == entry.id)
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
                // The compact absolute form, even in module-relative mode: the
                // tooltip is where the full address stays visible.
                return ui::format_absolute(entry.address);
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
            if (index.column() == address && entry.expression.empty() && !is_script(entry))
            {
                return ui::format_cell_address_full(address_mode_, module_spans_, entry.address);
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
            // The invalid (root) index is where a drop between rows lands.
            return Qt::ItemIsDropEnabled;
        }

        Qt::ItemFlags item_flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
        if (const auto row = static_cast<std::size_t>(index.row());
            row < table_.size() && is_script(table_.entries()[row]))
        {
            // A script row is edited through its dialog, never in the grid;
            // only its Active checkbox is interactive.
            if (index.column() == active)
            {
                item_flags |= Qt::ItemIsUserCheckable;
            }
            return item_flags;
        }
        switch (index.column())
        {
        case description:
        case value:
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
        int row = -1;
        for (const QModelIndex& index : indexes)
        {
            if (index.isValid() && index.row() >= 0 && (row < 0 || index.row() < row))
            {
                row = index.row();
            }
        }
        if (row < 0 || row >= rowCount())
        {
            return nullptr;
        }

        auto* data = new QMimeData;
        data->setData(QString::fromLatin1(kRowMimeType), QByteArray::number(row));
        return data;
    }

    bool
    AddressTableModel::canDropMimeData(const QMimeData* data, Qt::DropAction action, int, int, const QModelIndex&) const
    {
        return data != nullptr && action == Qt::MoveAction && data->hasFormat(QString::fromLatin1(kRowMimeType));
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
        const int count  = rowCount();
        if (!ok || source < 0 || source >= count)
        {
            return false;
        }

        // Qt hands the insertion row (the drop lands "before" it); a drop onto an
        // item arrives as a valid parent with row == -1.
        int insert_row = parent.isValid() ? parent.row() : (row >= 0 ? row : count);
        insert_row     = std::clamp(insert_row, 0, count);

        // The rows after the source shift up by one, so a downward move ends at
        // one index lower than the insertion row.
        const int destination = source < insert_row ? insert_row - 1 : insert_row;
        if (destination == source)
        {
            return false; // Dropped onto itself.
        }

        // beginMoveRows wants the destination child in the pre-move numbering:
        // one past the final index for a downward move.
        const int destination_child = source < destination ? destination + 1 : destination;
        if (!beginMoveRows(QModelIndex(), source, source, QModelIndex(), destination_child))
        {
            return false;
        }

        table_.move(static_cast<std::size_t>(source), static_cast<std::size_t>(destination));

        // The moved and shifted rows now stand for different entries, so their
        // cached readings are dropped; the next live pass refills them.
        const int first = std::min(source, destination);
        const int last  = std::max(source, destination);
        for (int index = first; index <= last && static_cast<std::size_t>(index) < cells_.size(); ++index)
        {
            cells_.cell(static_cast<std::size_t>(index)) = LiveCell {};
        }

        note_table_changed();
        endMoveRows();
        emit statusChanged(tr("Row moved."), false);
        return true;
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
                // The panel runs the hook and writes the flag once the verdict
                // arrives; repaint so the box snaps back until then.
                emit scriptActiveRequested(row, value.toInt() == Qt::Checked);
                emit dataChanged(index, index);
                return false;
            }
            // A script row is read-only in the grid: nothing here can submit a
            // write job for it.
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

        // A row whose entry identity changed must not keep the previous entry's
        // reading. The in-flight request is validated again on apply, so a stale
        // completion cannot land on the recycled row either.
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
                                  const bool  readable  = value_row && !entry.bytes.empty()
                                                       && !(writing_entry_.has_value() && *writing_entry_ == entry.id);
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
        const auto index_row = static_cast<std::size_t>(row);
        if (row < 0 || index_row >= table_.size())
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

        const auto&       entry = table_.entries()[index_row];
        const std::string input = text.toStdString();
        // Parse here as well so a malformed value keeps its detailed message.
        if (const auto parsed = scan::parse_value(entry.type, input, entry.hex); !parsed)
        {
            log::warning(log::category::ui, std::format("table value rejected: {}", parsed.error().message));
            emit statusChanged(tr("Value: %1").arg(to_qstring(parsed.error().message)), true);
            return false;
        }
        auto encoded = table_.encode_value(index_row, input);
        if (!encoded)
        {
            log::warning(log::category::ui, std::format("table value rejected: {}", input));
            emit statusChanged(tr("Value: invalid value."), true);
            return false;
        }

        const std::uint64_t  entry_id = entry.id;
        const std::uint64_t  address  = entry.address;
        const process::JobId job_id   = worker_.next_job_id();
        write_pending_                = job_id;
        writing_entry_                = entry_id;

        const bool submitted = worker_.submit_write(
            job_id,
            entry_id,
            address,
            std::move(*encoded),
            [this, entry_id, job_id, cached = *encoded](process::JobResult&& result)
            {
                if (write_pending_ != job_id)
                {
                    return; // Superseded or shut down.
                }
                write_pending_.reset();
                writing_entry_.reset();

                const auto& write = std::get<process::WriteResult>(result);
                if (write.error)
                {
                    log::warning(log::category::ui,
                                 std::format("table write failed: {}", process::describe(*write.error)));
                    emit dataChanged(index(0, 0), index(rowCount() - 1, column_count - 1));
                    emit statusChanged(tr("Write failed: %1").arg(to_qstring(process::describe(*write.error))), true);
                    return;
                }
                table_.apply_write(entry_id, std::move(cached));
                // Show what was just written without raising the change
                // highlight, so the user's own write is not flagged as a move.
                seed_live_write(entry_id);
                note_table_changed();
                emit dataChanged(index(0, 0), index(rowCount() - 1, column_count - 1));
                emit statusChanged(tr("Value written."), false);
            });
        if (!submitted)
        {
            write_pending_.reset();
            writing_entry_.reset();
            emit statusChanged(tr("Write unavailable."), true);
            return false;
        }

        emit dataChanged(index(row, value), index(row, value));
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
            if (current.id != last.id || current.active != last.active || current.kind != last.kind
                || current.description != last.description || current.address != last.address
                || current.expression != last.expression || current.type != last.type || current.hex != last.hex
                || current.bytes != last.bytes || current.script != last.script)
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

} // namespace slopkit::ui::models
