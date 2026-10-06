#include "ui/models/watch_hits_model.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

#include "ui/fonts.hpp"

namespace slopkit::ui::models
{

    WatchHitsModel::WatchHitsModel(QObject* parent, AddressText format)
        : QAbstractTableModel(parent), format_(std::move(format))
    {
    }

    void WatchHitsModel::sync(const debug::AccessWatch& watch)
    {
        // A new watch (a different address, or a fresh start at the same one)
        // replaces every row.
        if (address_ != watch.address() || (watch.state() == debug::WatchState::watching && !watching_))
        {
            beginResetModel();
            address_ = watch.address();
            rows_.clear();
            for (const debug::WatchHit& hit : watch.hits())
            {
                rows_.push_back(Row {hit.instruction, 0, QString(), hit.tid, hit.count});
            }
            endResetModel();
            watching_ = watch.state() == debug::WatchState::watching;
            return;
        }
        watching_ = watch.state() == debug::WatchState::watching;

        if (watch.hits().empty())
        {
            if (!rows_.empty())
            {
                beginResetModel();
                rows_.clear();
                endResetModel();
            }
            return;
        }

        for (const debug::WatchHit& hit : watch.hits())
        {
            const auto found = std::find_if(rows_.begin(),
                                            rows_.end(),
                                            [&hit](const Row& row)
                                            {
                                                return row.instruction == hit.instruction;
                                            });
            if (found != rows_.end())
            {
                if (found->count != hit.count || found->tid != hit.tid)
                {
                    found->count  = hit.count;
                    found->tid    = hit.tid;
                    const int row = static_cast<int>(std::distance(rows_.begin(), found));
                    emit      dataChanged(index(row, thread), index(row, count), {Qt::DisplayRole});
                }
                continue;
            }

            const int row = static_cast<int>(rows_.size());
            beginInsertRows(QModelIndex(), row, row);
            rows_.push_back(Row {hit.instruction, 0, QString(), hit.tid, hit.count});
            endInsertRows();
        }
    }

    bool WatchHitsModel::needs_text(int row) const
    {
        return row >= 0 && static_cast<std::size_t>(row) < rows_.size()
            && rows_[static_cast<std::size_t>(row)].text.isEmpty();
    }

    std::uint64_t WatchHitsModel::instruction_at(int row) const
    {
        return row >= 0 && static_cast<std::size_t>(row) < rows_.size()
                 ? rows_[static_cast<std::size_t>(row)].instruction
                 : 0;
    }

    std::uint64_t WatchHitsModel::display_instruction_at(int row) const
    {
        if (row < 0 || static_cast<std::size_t>(row) >= rows_.size())
        {
            return 0;
        }
        const Row& hit = rows_[static_cast<std::size_t>(row)];
        return hit.recovered != 0 ? hit.recovered : hit.instruction;
    }

    std::uint64_t WatchHitsModel::count_at(int row) const
    {
        return row >= 0 && static_cast<std::size_t>(row) < rows_.size() ? rows_[static_cast<std::size_t>(row)].count
                                                                        : 0;
    }

    QString WatchHitsModel::text_at(int row) const
    {
        if (row < 0 || static_cast<std::size_t>(row) >= rows_.size())
        {
            return {};
        }
        const Row& hit = rows_[static_cast<std::size_t>(row)];
        return hit.text.isEmpty() ? QStringLiteral("??") : hit.text;
    }

    void WatchHitsModel::set_text(int row, std::uint64_t recovered, QString instruction_text)
    {
        if (row < 0 || static_cast<std::size_t>(row) >= rows_.size())
        {
            return;
        }
        Row& hit      = rows_[static_cast<std::size_t>(row)];
        hit.recovered = recovered;
        hit.text      = std::move(instruction_text);
        emit dataChanged(index(row, instruction), index(row, text), {Qt::DisplayRole, Qt::ToolTipRole});
    }

    int WatchHitsModel::rowCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }

    int WatchHitsModel::columnCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : column_count;
    }

    QVariant WatchHitsModel::data(const QModelIndex& index, int role) const
    {
        if (!index.isValid() || index.row() >= static_cast<int>(rows_.size()))
        {
            return {};
        }
        const Row& hit = rows_[static_cast<std::size_t>(index.row())];

        if (role == Qt::ToolTipRole && index.column() == instruction)
        {
            return hit.recovered != 0 ? format_(hit.recovered) : QString();
        }
        // Every column holds machine data (addresses, decoded text, thread and
        // count), so the whole table lines up in the mono family.
        if (role == Qt::FontRole)
        {
            return QVariant::fromValue(mono_font());
        }
        if (role != Qt::DisplayRole)
        {
            return {};
        }

        switch (index.column())
        {
        case instruction:
            return format_(hit.recovered != 0 ? hit.recovered : hit.instruction);
        case text:
            return hit.text.isEmpty() ? QStringLiteral("??") : hit.text;
        case thread:
            return QString::number(hit.tid);
        case count:
            return QString::number(hit.count);
        default:
            return {};
        }
    }

    QVariant WatchHitsModel::headerData(int section, Qt::Orientation orientation, int role) const
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        {
            return {};
        }
        switch (section)
        {
        case instruction:
            return tr("Instruction");
        case text:
            return tr("Code");
        case thread:
            return tr("Thread");
        case count:
            return tr("Count");
        default:
            return {};
        }
    }

} // namespace slopkit::ui::models
