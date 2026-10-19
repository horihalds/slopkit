#include "ui/models/code_filter_model.hpp"

#include <utility>

#include "ui/components/elided_tooltip_delegate.hpp"
#include "ui/fonts.hpp"

namespace slopkit::ui::models
{
    CodeFilterModel::CodeFilterModel(QObject* parent, AddressText address_text)
        : QAbstractTableModel(parent), address_text_(std::move(address_text))
    {
    }

    void CodeFilterModel::set_rows(std::vector<CodeFilterRow> rows)
    {
        beginResetModel();
        rows_ = std::move(rows);
        endResetModel();
    }

    int CodeFilterModel::row_count() const
    {
        return static_cast<int>(rows_.size());
    }

    std::uint64_t CodeFilterModel::target_at(int row) const
    {
        return row >= 0 && static_cast<std::size_t>(row) < rows_.size() ? rows_[static_cast<std::size_t>(row)].target
                                                                        : 0;
    }

    int CodeFilterModel::rowCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }

    int CodeFilterModel::columnCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : column_count;
    }

    QVariant CodeFilterModel::data(const QModelIndex& index, int role) const
    {
        if (!index.isValid() || index.row() >= static_cast<int>(rows_.size()))
        {
            return {};
        }
        const CodeFilterRow& row = rows_[static_cast<std::size_t>(index.row())];

        if (role == Qt::ToolTipRole || role == widgets::kFullTextRole)
        {
            switch (index.column())
            {
            case address:
                return address_text_(row.target);
            case module_offset:
                return row.module_offset;
            case kind:
                return row.kind;
            default:
                break;
            }
        }
        if (role == Qt::FontRole)
        {
            return index.column() == state ? QVariant {} : QVariant::fromValue(mono_font());
        }
        if (role != Qt::DisplayRole)
        {
            return {};
        }

        switch (index.column())
        {
        case address:
            return address_text_(row.target);
        case module_offset:
            return row.module_offset;
        case kind:
            return row.kind;
        case state:
            return row.executed ? tr("executed") : tr("not seen");
        case executions:
            return QString::number(static_cast<qulonglong>(row.executions));
        default:
            return {};
        }
    }

    QVariant CodeFilterModel::headerData(int section, Qt::Orientation orientation, int role) const
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        {
            return {};
        }
        switch (section)
        {
        case address:
            return tr("Address");
        case module_offset:
            return tr("Module offset");
        case kind:
            return tr("Kind");
        case state:
            return tr("State");
        case executions:
            return tr("Executions");
        default:
            return {};
        }
    }
}
