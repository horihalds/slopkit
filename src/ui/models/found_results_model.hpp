#pragma once

#include <cstdint>
#include <vector>

#include "scan/engine.hpp"

#include <QAbstractTableModel>

namespace slopkit::ui::models
{

    // A half-open address range that marks the hits inside it as static: the
    // image span (code and data) of one loaded module.
    struct AddressRange
    {
        std::uint64_t start {};
        std::uint64_t end {};
    };

    // Address / Value / Previous rows over the display page of a scan snapshot.
    // The engine hands out at most one page of hits, so the model has no paging
    // of its own.
    class FoundResultsModel : public QAbstractTableModel
    {
        Q_OBJECT

    public:
        enum Column
        {
            address,
            value,
            previous,
            column_count,
        };

        explicit FoundResultsModel(QObject* parent = nullptr);

        [[nodiscard]] int      rowCount(const QModelIndex& parent = QModelIndex()) const override;
        [[nodiscard]] int      columnCount(const QModelIndex& parent = QModelIndex()) const override;
        [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
        [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
        void                   sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

        void set_snapshot(scan::ScanSnapshot snapshot, scan::ScanConfig config);
        void clear();

        // Sets the module image spans. Hits inside them are listed first and
        // their address is drawn in the success colour; the ranges are sorted
        // here and survive a later snapshot.
        void set_module_ranges(std::vector<AddressRange> ranges);

        // True when the address lies inside one of the module image spans.
        [[nodiscard]] bool is_static(std::uint64_t address) const;

        // The hit a row currently shows, or nullptr when the row is out of date.
        [[nodiscard]] const scan::ScanHit* hit_at(int row) const;

    private:
        void rebuild_order();
        void refresh_static_flags();
        void apply_sort();

        scan::ScanSnapshot        snapshot_;
        scan::ScanConfig          config_;
        std::vector<int>          order_;
        std::vector<bool>         static_hits_;
        std::vector<AddressRange> module_ranges_;
        int                       sort_column_ {address};
        Qt::SortOrder             sort_order_ {Qt::AscendingOrder};
    };

} // namespace slopkit::ui::models
