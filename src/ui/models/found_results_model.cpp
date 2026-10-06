#include "ui/models/found_results_model.hpp"

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <QBrush>

#include "scan/value.hpp"
#include "ui/fonts.hpp"
#include "ui/text.hpp"
#include "ui/theme.hpp"

namespace slopkit::ui::models
{

    namespace
    {
        int compare_bytes(const std::vector<std::byte>& lhs, const std::vector<std::byte>& rhs)
        {
            return lhs < rhs ? -1 : (rhs < lhs ? 1 : 0);
        }

        int compare_addresses(std::uint64_t lhs, std::uint64_t rhs)
        {
            return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
        }

        // Ordering tier: main-image hits (0) before other module-backed hits (1)
        // before the dynamic ones (2).
        int hit_tier(const ui::ModuleSpans& spans, std::uint64_t address)
        {
            if (const ui::ModuleSpan* main = spans.main();
                main != nullptr && address >= main->base && address < main->end)
            {
                return 0;
            }
            return spans.containing(address) != nullptr ? 1 : 2;
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
            {
                const LiveCell* cell = live_cell_at(index.row());
                if (cell != nullptr && cell->has_reading)
                {
                    if (!cell->readable)
                    {
                        return tr("?");
                    }
                    if (cell->bytes.has_value())
                    {
                        return to_qstring(scan::format_value(config_.value_type, *cell->bytes, config_.hex));
                    }
                }
                return to_qstring(scan::format_value(config_.value_type, hit.value, config_.hex));
            }
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
            if (index.column() == value)
            {
                const LiveCell* cell = live_cell_at(index.row());
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
        case Qt::ToolTipRole:
            if (index.column() == value)
            {
                const LiveCell* cell = live_cell_at(index.row());
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
        live_.clear();
        live_pending_.clear();
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
        live_.clear();
        live_pending_.clear();
        if (!order_.empty())
        {
            emit dataChanged(index(0, address),
                             index(static_cast<int>(order_.size()) - 1, value),
                             {Qt::DisplayRole, Qt::ForegroundRole});
        }
    }

    QString FoundResultsModel::address_text(std::uint64_t address) const
    {
        if (const auto relative = ui::module_relative_text(address_mode_, module_spans_, address); relative.has_value())
        {
            return *relative;
        }
        return ui::format_absolute(address);
    }

    QString FoundResultsModel::copy_text(int row, CopyFormat format) const
    {
        const scan::ScanHit* hit = hit_at(row);
        if (hit == nullptr)
        {
            return {};
        }
        if (format == CopyFormat::absolute)
        {
            return ui::format_absolute(hit->address);
        }
        if (format == CopyFormat::module_relative)
        {
            if (const auto relative =
                    ui::module_relative_text(ui::AddressMode::module_relative, module_spans_, hit->address);
                relative.has_value())
            {
                return *relative;
            }
            return ui::format_absolute(hit->address);
        }
        return address_text(hit->address) + QStringLiteral(": ")
             + to_qstring(scan::format_value(config_.value_type, hit->value, config_.hex));
    }

    bool FoundResultsModel::is_static(std::uint64_t address) const
    {
        return module_spans_.containing(address) != nullptr;
    }

    bool FoundResultsModel::is_main_hit(std::uint64_t address) const
    {
        const ui::ModuleSpan* main = module_spans_.main();
        return main != nullptr && address >= main->base && address < main->end;
    }

    std::vector<LiveRequest> FoundResultsModel::next_live_request()
    {
        live_pending_.clear();
        const std::size_t shown = order_.size();
        if (live_.size() != shown)
        {
            live_.assign(shown, LiveCell {});
        }

        std::vector<LiveRequest> requests;
        requests.reserve(shown);
        live_pending_.reserve(shown);
        for (std::size_t row = 0; row < shown; ++row)
        {
            const auto  hit_index = static_cast<std::size_t>(order_[row]);
            const auto& hit       = (*hits_)[hit_index];
            if (hit.value.empty())
            {
                continue; // A zero-width hit has nothing to read.
            }
            requests.push_back(LiveRequest {.id = row, .address = hit.address, .size = hit.value.size()});
            live_pending_.push_back(LivePending {.row = row, .hit_index = order_[row], .address = hit.address});
        }
        return requests;
    }

    void FoundResultsModel::apply_live_readings(std::span<const LiveReading> readings)
    {
        const std::size_t count = std::min(readings.size(), live_pending_.size());

        std::optional<int> run_start;
        std::optional<int> run_end;
        const auto         flush = [&]()
        {
            if (run_start.has_value())
            {
                emit dataChanged(
                    index(*run_start, value), index(*run_end, value), {Qt::DisplayRole, Qt::ForegroundRole});
                run_start.reset();
                run_end.reset();
            }
        };

        for (std::size_t i = 0; i < count; ++i)
        {
            const LivePending& pending = live_pending_[i];
            const LiveReading& reading = readings[i];
            const std::size_t  row     = pending.row;
            if (reading.id != row || row >= order_.size() || row >= live_.size())
            {
                continue; // Out of range; leave the row alone.
            }
            if (order_[row] != pending.hit_index
                || (*hits_)[static_cast<std::size_t>(order_[row])].address != pending.address)
            {
                continue; // The row moved to another hit since the request.
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

            const int row_index = static_cast<int>(row);
            if (was_state != cell.has_reading || was_read != cell.readable || was_chg != cell.changed
                || was_bytes != cell.bytes)
            {
                if (!run_start.has_value())
                {
                    run_start = row_index;
                }
                run_end = row_index;
            }
            else
            {
                flush();
            }
        }
        flush();

        live_pending_.clear();
    }

    const FoundResultsModel::LiveCell* FoundResultsModel::live_cell_at(int row) const
    {
        if (row < 0 || row >= static_cast<int>(live_.size()))
        {
            return nullptr;
        }
        return &live_[static_cast<std::size_t>(row)];
    }

    // Shows the top `scan::kDisplayPage` rows of the whole-list ordering: the
    // main-image hits first, then the other module-backed hits, then the dynamic
    // ones, each group ordered by the current sort column with the address as the
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
                              // The tier groups the main image, the other
                              // module-backed hits and the dynamic hits; the
                              // sort order only orders within each group.
                              const int left_tier =
                                  hit_tier(module_spans_, (*hits_)[static_cast<std::size_t>(lhs)].address);
                              const int right_tier =
                                  hit_tier(module_spans_, (*hits_)[static_cast<std::size_t>(rhs)].address);
                              if (left_tier != right_tier)
                              {
                                  return left_tier < right_tier;
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
        live_.clear();
        live_pending_.clear();
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
