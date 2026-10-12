#include "ui/models/referenced_string_model.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "ui/components/elided_tooltip_delegate.hpp"
#include "ui/fonts.hpp"

namespace slopkit::ui::models
{
    ReferencedStringModel::ReferencedStringModel(QObject* parent, AddressText address_text)
        : QAbstractTableModel(parent), address_text_(std::move(address_text))
    {
    }

    void ReferencedStringModel::set_rows(std::vector<ReferencedStringRow> rows)
    {
        beginResetModel();
        rows_ = std::move(rows);
        endResetModel();
    }

    int ReferencedStringModel::row_count() const
    {
        return static_cast<int>(rows_.size());
    }

    std::uint64_t ReferencedStringModel::instruction_at(int row) const
    {
        return row >= 0 && static_cast<std::size_t>(row) < rows_.size()
                 ? rows_[static_cast<std::size_t>(row)].instruction
                 : 0;
    }

    int ReferencedStringModel::rowCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }

    int ReferencedStringModel::columnCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : column_count;
    }

    QVariant ReferencedStringModel::data(const QModelIndex& index, int role) const
    {
        if (!index.isValid() || index.row() >= static_cast<int>(rows_.size()))
        {
            return {};
        }
        const ReferencedStringRow& row = rows_[static_cast<std::size_t>(index.row())];

        if (index.column() == string && (role == Qt::ToolTipRole || role == widgets::kFullTextRole))
        {
            return row.text;
        }
        if (role == Qt::FontRole)
        {
            return index.column() == module ? QVariant {} : QVariant::fromValue(mono_font());
        }
        if (role != Qt::DisplayRole)
        {
            return {};
        }

        switch (index.column())
        {
        case string:
            return row.text;
        case address:
            return address_text_(row.string_address);
        case module:
            return row.module;
        case referenced_by:
            return address_text_(row.instruction);
        default:
            return {};
        }
    }

    QVariant ReferencedStringModel::headerData(int section, Qt::Orientation orientation, int role) const
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        {
            return {};
        }
        switch (section)
        {
        case string:
            return tr("String");
        case address:
            return tr("Address");
        case module:
            return tr("Module");
        case referenced_by:
            return tr("Referenced by");
        default:
            return {};
        }
    }

}
