#include "ui/models/instruction_access_model.hpp"

#include <algorithm>
#include <utility>

namespace slopkit::ui::models
{

    InstructionAccessModel::InstructionAccessModel(QObject* parent, AddressText format)
        : QAbstractTableModel(parent), format_(std::move(format))
    {
    }

    void InstructionAccessModel::set(std::vector<ui::ResolvedAccess> accesses)
    {
        beginResetModel();
        rows_.clear();
        for (const ui::ResolvedAccess& access : accesses)
        {
            rows_.push_back(Row {access.address, access.operand, access.width, access.writes, access.resolved});
        }
        endResetModel();
    }

    int InstructionAccessModel::row_count() const
    {
        return static_cast<int>(rows_.size());
    }

    std::uint64_t InstructionAccessModel::address_at(int row) const
    {
        return row >= 0 && static_cast<std::size_t>(row) < rows_.size() ? rows_[static_cast<std::size_t>(row)].address
                                                                        : 0;
    }

    std::size_t InstructionAccessModel::width_at(int row) const
    {
        return row >= 0 && static_cast<std::size_t>(row) < rows_.size() ? rows_[static_cast<std::size_t>(row)].width
                                                                        : 0;
    }

    bool InstructionAccessModel::resolved_at(int row) const
    {
        return row >= 0 && static_cast<std::size_t>(row) < rows_.size()
            && rows_[static_cast<std::size_t>(row)].resolved;
    }

    bool InstructionAccessModel::any_resolved() const
    {
        return std::any_of(rows_.begin(),
                           rows_.end(),
                           [](const Row& row)
                           {
                               return row.resolved;
                           });
    }

    int InstructionAccessModel::rowCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }

    int InstructionAccessModel::columnCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : column_count;
    }

    QVariant InstructionAccessModel::data(const QModelIndex& index, int role) const
    {
        if (!index.isValid() || index.row() >= static_cast<int>(rows_.size()))
        {
            return {};
        }
        const Row& row = rows_[static_cast<std::size_t>(index.row())];

        if (role == Qt::ToolTipRole && index.column() == address && !row.resolved)
        {
            return tr("The register it needs is not available yet.");
        }
        if (role != Qt::DisplayRole)
        {
            return {};
        }

        switch (index.column())
        {
        case address:
            return row.resolved ? format_(row.address) : QStringLiteral("?");
        case operand:
            return row.operand;
        case width:
            return QString::number(row.width);
        case access:
            return row.writes ? tr("write") : tr("read");
        default:
            return {};
        }
    }

    QVariant InstructionAccessModel::headerData(int section, Qt::Orientation orientation, int role) const
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        {
            return {};
        }
        switch (section)
        {
        case address:
            return tr("Address");
        case operand:
            return tr("Operand");
        case width:
            return tr("Width");
        case access:
            return tr("Access");
        default:
            return {};
        }
    }

} // namespace slopkit::ui::models
