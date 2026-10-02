#pragma once

#include "scan/engine.hpp"
#include "table/address_table.hpp"

namespace slopkit::ui::panels
{

    // The left half of the middle zone: the `Found: N` result list. It only
    // reads the engine's snapshot and appends double-clicked hits to the address
    // table.
    class FoundListPanel
    {
    public:
        FoundListPanel(scan::ScanEngine& engine, table::AddressTable& table);

        void draw();

    private:
        void add_to_table(const scan::ScanHit& hit, const scan::ScanConfig& config);

        scan::ScanEngine&    engine_;
        table::AddressTable& table_;
        int                  selected_ {-1};
    };

} // namespace slopkit::ui::panels
