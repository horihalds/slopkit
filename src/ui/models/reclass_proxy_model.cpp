#include "ui/models/reclass_proxy_model.hpp"

#include "ui/components/elided_tooltip_delegate.hpp"
#include "ui/models/reclass_structure_model.hpp"
#include "ui/models/reclass_value_model.hpp"

namespace slopkit::ui::models
{
    ReclassProxyModel::ReclassProxyModel(ReclassStructureModel& structure, ReclassValueModel& values, QObject* parent)
        : QIdentityProxyModel(parent), structure_(structure), values_(values)
    {
        setSourceModel(&structure);
    }

    QVariant ReclassProxyModel::data(const QModelIndex& index, int role) const
    {
        const ReclassRow* row = structure_.row_for(mapToSource(index));
        if (row != nullptr && (role == Qt::DisplayRole || role == Qt::EditRole || role == widgets::kFullTextRole))
        {
            if (index.column() == ReclassStructureModel::address)
            {
                return values_.address_text(*row);
            }
            if (index.column() == ReclassStructureModel::value)
            {
                return values_.value_text(*row);
            }
        }
        return QIdentityProxyModel::data(index, role);
    }

    Qt::ItemFlags ReclassProxyModel::flags(const QModelIndex& index) const
    {
        Qt::ItemFlags result = QIdentityProxyModel::flags(index);
        if (index.column() == ReclassStructureModel::value)
        {
            const ReclassRow* row = structure_.row_for(mapToSource(index));
            if (row != nullptr && values_.editable(*row))
            {
                result |= Qt::ItemIsEditable;
            }
        }
        return result;
    }

    bool ReclassProxyModel::setData(const QModelIndex& index, const QVariant& value, int role)
    {
        if (role != Qt::EditRole || index.column() != ReclassStructureModel::value)
        {
            return false;
        }
        const ReclassRow* row = structure_.row_for(mapToSource(index));
        return row != nullptr && values_.write_value(*row, value.toString());
    }

    void ReclassProxyModel::refresh_values()
    {
        refresh({});
    }

    void ReclassProxyModel::refresh(const QModelIndex& parent)
    {
        const int rows = rowCount(parent);
        for (int row = 0; row < rows; ++row)
        {
            const QModelIndex first = index(row, ReclassStructureModel::address, parent);
            const QModelIndex last  = index(row, ReclassStructureModel::value, parent);
            if (first.isValid() && last.isValid())
            {
                emit dataChanged(first, last);
            }
            refresh(first);
        }
    }

}
