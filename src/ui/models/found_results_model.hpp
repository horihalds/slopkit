#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "process/types.hpp"
#include "scan/engine.hpp"
#include "ui/address_format.hpp"
#include "ui/live_values.hpp"
#include "ui/models/copy_format.hpp"
#include "ui/models/live_cells.hpp"

#include <QAbstractTableModel>

namespace slopkit::ui::models
{
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

        void set_modules(std::vector<process::ModuleInfo> modules);

        void set_address_mode(ui::AddressMode mode);

        [[nodiscard]] QString address_text(std::uint64_t address) const;

        [[nodiscard]] QString copy_text(int row, CopyFormat format) const;

        [[nodiscard]] bool is_static(std::uint64_t address) const;

        [[nodiscard]] bool is_main_hit(std::uint64_t address) const;

        [[nodiscard]] std::vector<LiveRequest> next_live_request();

        void apply_live_readings(std::span<const LiveReading> readings);

        [[nodiscard]] const scan::ScanHit* hit_at(int row) const;

    private:
        void                          rebuild_window();
        void                          refresh_static_flags();
        [[nodiscard]] const LiveCell* live_cell_at(int row) const;

        std::shared_ptr<const std::vector<scan::ScanHit>> hits_;
        scan::ScanConfig                                  config_;
        std::vector<int>                                  order_;
        std::vector<bool>                                 static_hits_;
        LiveCells                                         cells_;
        ui::ModuleSpans                                   module_spans_;
        ui::AddressMode                                   address_mode_ {ui::AddressMode::module_relative};
        int                                               sort_column_ {address};
        Qt::SortOrder                                     sort_order_ {Qt::AscendingOrder};
    };

}
