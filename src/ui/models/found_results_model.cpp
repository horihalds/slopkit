#include "ui/models/found_results_model.hpp"

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <string_view>
#include <utility>
#include <vector>

#include <QBrush>

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

        int compare_bytes(const std::vector<std::byte>& lhs, const std::vector<std::byte>& rhs)
        {
            return lhs < rhs ? -1 : (rhs < lhs ? 1 : 0);
        }

        int compare_addresses(std::uint64_t lhs, std::uint64_t rhs)
        {
            return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
        }
    } // namespace

    FoundResultsModel::FoundResultsModel(QObject* parent)
        : QAbstractTableModel(parent), hits_(std::make_shared<const std::vector<scan::ScanHit>>())
    {
    }

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

        const auto& hit = (*hits_)[static_cast<std::size_t>(order_[static_cast<std::size_t>(index.row())])];

        switch (role)
        {
        case Qt::DisplayRole:
            switch (index.column())
            {
            case address:
                return address_text(hit.address);
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
        case Qt::ForegroundRole:
            if (index.column() == address && static_hits_[static_cast<std::size_t>(index.row())])
            {
                return QBrush(active_theme().success);
            }
            break;
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
        rebuild_window();
        endResetModel();
    }

    void FoundResultsModel::set_snapshot(scan::ScanSnapshot snapshot, scan::ScanConfig config)
    {
        beginResetModel();
        hits_ = std::move(snapshot.result_hits);
        if (!hits_)
        {
            hits_ = std::make_shared<const std::vector<scan::ScanHit>>(std::move(snapshot.hits));
        }
        config_ = std::move(config);
        rebuild_window();
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
        return &(*hits_)[static_cast<std::size_t>(order_[static_cast<std::size_t>(row)])];
    }

    void FoundResultsModel::set_modules(std::vector<process::ModuleInfo> modules)
    {
        ui::ModuleSpans spans;
        spans.set_modules(modules);
        if (spans == module_spans_)
        {
            return;
        }

        beginResetModel();
        module_spans_ = std::move(spans);
        rebuild_window();
        endResetModel();
    }

    void FoundResultsModel::set_address_mode(ui::AddressMode mode)
    {
        if (address_mode_ == mode)
        {
            return;
        }
        address_mode_ = mode;
        if (!order_.empty())
        {
            emit dataChanged(index(0, address), index(static_cast<int>(order_.size()) - 1, address), {Qt::DisplayRole});
        }
    }

    QString FoundResultsModel::address_text(std::uint64_t address) const
    {
        if (const auto relative = ui::module_relative_text(address_mode_, module_spans_, address); relative.has_value())
        {
            return *relative;
        }
        return QStringLiteral("0x") + QString::number(address, 16).toUpper();
    }

    bool FoundResultsModel::is_static(std::uint64_t address) const
    {
        return module_spans_.containing(address) != nullptr;
    }

    // Shows the top `scan::kDisplayPage` rows of the whole-list ordering: the
    // static hits first, then the current sort column with the address as the
    // tie-breaker. The whole source range is considered through std::partial_sort,
    // so only the visible window is ordered and kept.
    void FoundResultsModel::rebuild_window()
    {
        const std::size_t total = hits_->size();
        const std::size_t shown = std::min<std::size_t>(total, scan::kDisplayPage);

        order_.resize(total);
        std::iota(order_.begin(), order_.end(), 0);
        std::partial_sort(order_.begin(),
                          order_.begin() + static_cast<std::ptrdiff_t>(shown),
                          order_.end(),
                          [this](int lhs, int rhs)
                          {
                              // Static hits always group above the others; the
                              // sort order only orders within each group.
                              const bool left_static  = is_static((*hits_)[static_cast<std::size_t>(lhs)].address);
                              const bool right_static = is_static((*hits_)[static_cast<std::size_t>(rhs)].address);
                              if (left_static != right_static)
                              {
                                  return left_static;
                              }

                              const auto& left  = (*hits_)[static_cast<std::size_t>(lhs)];
                              const auto& right = (*hits_)[static_cast<std::size_t>(rhs)];

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
        order_.resize(shown);
        refresh_static_flags();
    }

    void FoundResultsModel::refresh_static_flags()
    {
        static_hits_.resize(order_.size());
        for (std::size_t i = 0; i < order_.size(); ++i)
        {
            static_hits_[i] = is_static((*hits_)[static_cast<std::size_t>(order_[i])].address);
        }
    }

} // namespace slopkit::ui::models
