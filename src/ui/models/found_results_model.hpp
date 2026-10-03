#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "process/types.hpp"
#include "scan/engine.hpp"
#include "ui/address_format.hpp"

#include <QAbstractTableModel>

namespace slopkit::ui::models
{

    // Address / Value / Previous rows over a scan snapshot. The ordering is
    // computed over the whole stored result set and the model shows the top
    // display page of that ordering.
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

        // Sets the module image spans. Hits inside the main image are listed
        // before the other module-backed hits, which are listed before the
        // dynamic ones; their address is drawn in the success colour. The spans
        // are sorted here and survive a later snapshot.
        void set_modules(std::vector<process::ModuleInfo> modules);

        // Chooses how static addresses are shown in the Address column.
        void set_address_mode(ui::AddressMode mode);

        // The rendered Address column text for `address`.
        [[nodiscard]] QString address_text(std::uint64_t address) const;

        // True when the address lies inside one of the module image spans.
        [[nodiscard]] bool is_static(std::uint64_t address) const;

        // True when the address lies inside the target's main image span.
        [[nodiscard]] bool is_main_hit(std::uint64_t address) const;

        // The hit a row currently shows, or nullptr when the row is out of date.
        [[nodiscard]] const scan::ScanHit* hit_at(int row) const;

    private:
        void rebuild_window();
        void refresh_static_flags();

        // The source rows: the engine's whole stored result set once a scan
        // finished, or a local copy of the running incremental page. Never null.
        std::shared_ptr<const std::vector<scan::ScanHit>> hits_;
        scan::ScanConfig                                  config_;
        // On-screen rows: the top `kDisplayPage` source indices of the whole-list
        // ordering.
        std::vector<int>                                  order_;
        // Parallel to `order_`, not to `hits_`.
        std::vector<bool>                                 static_hits_;
        ui::ModuleSpans                                   module_spans_;
        ui::AddressMode                                   address_mode_ {ui::AddressMode::module_relative};
        int                                               sort_column_ {address};
        Qt::SortOrder                                     sort_order_ {Qt::AscendingOrder};
    };

} // namespace slopkit::ui::models
