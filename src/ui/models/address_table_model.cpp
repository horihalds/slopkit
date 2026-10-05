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
#include "ui/fonts.hpp"
#include "ui/theme.hpp"

namespace slopkit::ui::models
{

    namespace
    {
        QString to_qstring(std::string_view text)
        {
            return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
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
                if (!entry.expression.empty())
                {
                    return to_qstring(entry.expression);
                }
                return address_text(entry.address);
            case type:
                return to_qstring(scan::describe(entry.type));
            case value:
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
            if (index.column() == frozen)
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
            if (index.column() == frozen)
            {
                return static_cast<int>(Qt::AlignCenter);
            }
            return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
        case Qt::ToolTipRole:
            if (index.column() == frozen)
            {
                return tr("Freeze this value");
            }
            if (index.column() == address && !entry.expression.empty())
            {
                return QStringLiteral("0x") + QString::number(entry.address, 16).toUpper();
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
            case frozen:
                return tr("Frozen");
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
            return Qt::NoItemFlags;
        }

        Qt::ItemFlags item_flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        switch (index.column())
        {
        case description:
        case value:
            item_flags |= Qt::ItemIsEditable;
            break;
        case frozen:
            item_flags |= Qt::ItemIsUserCheckable;
            break;
        default:
            break;
        }
        return item_flags;
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

        if (role == Qt::CheckStateRole && index.column() == frozen)
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
            live_.assign(entries.size(), LiveCell {});
            live_pending_.clear();
            note_table_changed();
            endResetModel();
            return;
        }

        // A row whose entry identity changed must not keep the previous entry's
        // reading. The in-flight request is validated again on apply, so a stale
        // completion cannot land on the recycled row either.
        for (std::size_t row = 0; row < entries.size(); ++row)
        {
            if (last_entries_[row].id != entries[row].id && row < live_.size())
            {
                live_[row] = LiveCell {};
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
        if (live_.size() != entries.size())
        {
            live_.assign(entries.size(), LiveCell {});
        }
        live_pending_.clear();

        std::vector<LiveRequest> requests;
        requests.reserve(entries.size());
        live_pending_.reserve(entries.size());

        for (std::size_t row = 0; row < entries.size(); ++row)
        {
            const auto& entry = entries[row];
            if (entry.bytes.empty())
            {
                continue; // A zero-width entry has nothing to read.
            }
            if (writing_entry_.has_value() && *writing_entry_ == entry.id)
            {
                continue; // Keep the placeholder until the write completes.
            }
            requests.push_back(LiveRequest {.id = row, .address = entry.address, .size = entry.bytes.size()});
            live_pending_.push_back(LivePending {.row = row, .entry_id = entry.id});
        }
        return requests;
    }

    void AddressTableModel::apply_live_readings(std::span<const LiveReading> readings)
    {
        const std::size_t count   = std::min(readings.size(), live_pending_.size());
        const auto&       entries = table_.entries();

        std::optional<std::size_t> run_start;
        std::optional<std::size_t> run_end;
        const auto                 flush = [&]()
        {
            if (run_start.has_value())
            {
                emit dataChanged(index(static_cast<int>(*run_start), value),
                                 index(static_cast<int>(*run_end), value),
                                 {Qt::DisplayRole, Qt::ForegroundRole});
                run_start.reset();
                run_end.reset();
            }
        };

        for (std::size_t i = 0; i < count; ++i)
        {
            const LivePending& pending = live_pending_[i];
            const LiveReading& reading = readings[i];
            const std::size_t  row     = pending.row;
            if (reading.id != row || row >= entries.size() || row >= live_.size())
            {
                continue; // Out of range; leave the row alone.
            }
            if (entries[row].id != pending.entry_id)
            {
                continue; // The row changed identity since the request.
            }

            LiveCell&  cell      = live_[row];
            const bool was_state = cell.has_reading;
            const bool was_read  = cell.readable;
            const bool was_chg   = cell.changed;
            const auto was_bytes = cell.bytes;

            const bool readable = reading.readable && !reading.bytes.empty();
            if (readable)
            {
                cell.changed = cell.bytes.has_value() && *cell.bytes != reading.bytes;
                cell.bytes   = reading.bytes;
            }
            else
            {
                cell.changed = false;
            }
            cell.has_reading = true;
            cell.readable    = readable;

            if (was_state != cell.has_reading || was_read != cell.readable || was_chg != cell.changed
                || was_bytes != cell.bytes)
            {
                if (!run_start.has_value())
                {
                    run_start = row;
                }
                run_end = row;
            }
            else
            {
                flush();
            }
        }
        flush();

        live_pending_.clear();
    }

    const AddressTableModel::LiveCell* AddressTableModel::live_cell_at(std::size_t row) const
    {
        return row < live_.size() ? &live_[row] : nullptr;
    }

    void AddressTableModel::seed_live_write(std::uint64_t entry_id)
    {
        const auto& entries = table_.entries();
        for (std::size_t row = 0; row < entries.size() && row < live_.size(); ++row)
        {
            if (entries[row].id != entry_id)
            {
                continue;
            }
            LiveCell& cell   = live_[row];
            cell.has_reading = true;
            cell.readable    = true;
            cell.bytes       = entries[row].bytes;
            cell.changed     = false;
            return;
        }
    }

    QString AddressTableModel::address_text(std::uint64_t address) const
    {
        if (const auto relative = ui::module_relative_text(address_mode_, module_spans_, address); relative.has_value())
        {
            return *relative;
        }
        return QStringLiteral("0x") + QString::number(address, 16).toUpper();
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
            if (current.id != last.id || current.active != last.active || current.description != last.description
                || current.address != last.address || current.expression != last.expression || current.type != last.type
                || current.hex != last.hex || current.bytes != last.bytes)
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

} // namespace slopkit::ui::models
