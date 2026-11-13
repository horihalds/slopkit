#include "ui/models/pointer_scan_model.hpp"

#include <utility>

#include "ui/components/elided_tooltip_delegate.hpp"
#include "ui/fonts.hpp"

namespace slopkit::ui::models
{
    PointerScanModel::PointerScanModel(QObject* parent, AddressText address_text)
        : QAbstractTableModel(parent), address_text_(std::move(address_text))
    {
    }

    void PointerScanModel::set_rows(std::vector<PointerScanRow> rows)
    {
        beginResetModel();
        rows_ = std::move(rows);
        endResetModel();
    }

    int PointerScanModel::row_count() const
    {
        return static_cast<int>(rows_.size());
    }

    std::uint64_t PointerScanModel::base_at(int row) const
    {
        return row >= 0 && static_cast<std::size_t>(row) < rows_.size() ? rows_[static_cast<std::size_t>(row)].base : 0;
    }

    int PointerScanModel::rowCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }

    int PointerScanModel::columnCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : column_count;
    }

    QVariant PointerScanModel::data(const QModelIndex& index, int role) const
    {
        if (!index.isValid() || index.row() >= static_cast<int>(rows_.size()))
        {
            return {};
        }
        const PointerScanRow& row = rows_[static_cast<std::size_t>(index.row())];

        if (role == Qt::ToolTipRole || role == widgets::kFullTextRole)
        {
            return index.column() == base ? address_text_(row.base) : row.offsets;
        }
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
        case base:
            return address_text_(row.base);
        case offsets:
            return row.offsets;
        default:
            return {};
        }
    }

    QVariant PointerScanModel::headerData(int section, Qt::Orientation orientation, int role) const
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        {
            return {};
        }
        switch (section)
        {
        case base:
            return tr("Static base");
        case offsets:
            return tr("Offsets");
        default:
            return {};
        }
    }
}
