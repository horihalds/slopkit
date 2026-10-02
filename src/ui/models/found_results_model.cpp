#include "ui/models/found_results_model.hpp"

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <string_view>
#include <utility>
#include <vector>

#include "scan/value.hpp"
#include "ui/fonts.hpp"

namespace slopkit::ui::models
{

    namespace
    {
        QString to_qstring(std::string_view text)
        {
            return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
        }

        int compare_bytes(const std::vector<std::byte>& lhs, const std::vector<std::byte>& rhs)
        {
            return lhs < rhs ? -1 : (rhs < lhs ? 1 : 0);
        }

        int compare_addresses(std::uint64_t lhs, std::uint64_t rhs)
        {
            return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
        }
    } // namespace

    FoundResultsModel::FoundResultsModel(QObject* parent) : QAbstractTableModel(parent) {}

    int FoundResultsModel::rowCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : static_cast<int>(order_.size());
    }

    int FoundResultsModel::columnCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : column_count;
    }

    QVariant FoundResultsModel::data(const QModelIndex& index, int role) const
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(order_.size()))
        {
            return {};
        }

        const auto& hit = snapshot_.hits[static_cast<std::size_t>(order_[static_cast<std::size_t>(index.row())])];

        switch (role)
        {
        case Qt::DisplayRole:
            switch (index.column())
            {
            case address:
                return QStringLiteral("0x") + QString::number(hit.address, 16).toUpper();
            case value:
                return to_qstring(scan::format_value(config_.value_type, hit.value, config_.hex));
            case previous:
                return hit.previous.empty()
                         ? QString()
                         : to_qstring(scan::format_value(config_.value_type, hit.previous, config_.hex));
            default:
                break;
            }
            break;
        case Qt::FontRole:
            return QVariant::fromValue(mono_font());
        case Qt::TextAlignmentRole:
            return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
        default:
            break;
        }
        return {};
    }

    QVariant FoundResultsModel::headerData(int section, Qt::Orientation orientation, int role) const
    {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole)
        {
            switch (section)
            {
            case address:
                return tr("Address");
            case value:
                return tr("Value");
            case previous:
                return tr("Previous");
            default:
                break;
            }
        }
        return QAbstractTableModel::headerData(section, orientation, role);
    }

    void FoundResultsModel::sort(int column, Qt::SortOrder order)
    {
        sort_column_ = column;
        sort_order_  = order;

        beginResetModel();
        apply_sort();
        endResetModel();
    }

    void FoundResultsModel::set_snapshot(scan::ScanSnapshot snapshot, scan::ScanConfig config)
    {
        beginResetModel();
        snapshot_ = std::move(snapshot);
        config_   = std::move(config);
        rebuild_order();
        apply_sort();
        endResetModel();
    }

    void FoundResultsModel::clear()
    {
        set_snapshot(scan::ScanSnapshot {}, scan::ScanConfig {});
    }

    const scan::ScanHit* FoundResultsModel::hit_at(int row) const
    {
        if (row < 0 || row >= static_cast<int>(order_.size()))
        {
            return nullptr;
        }
        return &snapshot_.hits[static_cast<std::size_t>(order_[static_cast<std::size_t>(row)])];
    }

    void FoundResultsModel::rebuild_order()
    {
        order_.resize(snapshot_.hits.size());
        std::iota(order_.begin(), order_.end(), 0);
    }

    void FoundResultsModel::apply_sort()
    {
        const auto& hits = snapshot_.hits;
        std::stable_sort(order_.begin(),
                         order_.end(),
                         [&](int lhs, int rhs)
                         {
                             const auto& left  = hits[static_cast<std::size_t>(lhs)];
                             const auto& right = hits[static_cast<std::size_t>(rhs)];

                             int by = compare_addresses(left.address, right.address);
                             switch (sort_column_)
                             {
                             case value:
                                 by = compare_bytes(left.value, right.value);
                                 break;
                             case previous:
                                 by = compare_bytes(left.previous, right.previous);
                                 break;
                             default:
                                 break;
                             }
                             if (by == 0)
                             {
                                 by = compare_addresses(left.address, right.address);
                             }
                             return sort_order_ == Qt::AscendingOrder ? by < 0 : by > 0;
                         });
    }

} // namespace slopkit::ui::models
