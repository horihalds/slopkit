#pragma once

#include "scan/engine.hpp"
#include "table/address_table.hpp"

#include <QWidget>

class QLabel;
class QPushButton;
class QTableView;

namespace slopkit::ui::models
{
    class FoundResultsModel;
} // namespace slopkit::ui::models

namespace slopkit::ui::widgets
{
    class StatusLabel;
} // namespace slopkit::ui::widgets

namespace slopkit::ui::panels
{

    // The left half of the middle zone: the `Found: N` result list. It only
    // reads the engine's snapshot and appends double-clicked hits to the address
    // table. Its entry row hosts the Memory View and Add Address buttons.
    class FoundListPanel : public QWidget
    {
        Q_OBJECT

    public:
        FoundListPanel(scan::ScanEngine& engine, table::AddressTable& table, QWidget* parent = nullptr);

        // Polled by the window's tick; only a changed snapshot reaches the model,
        // so the selection and the sort survive an idle poll.
        void refresh();

        // Enables the Memory View button only while a process is attached.
        void set_target_attached(bool attached);

    signals:
        // A request to open the Memory Viewer at the main module's entry point
        // (or its base when the plugin reports none).
        void memoryViewRequested();

        // A request to open the Add Address dialog.
        void addAddressRequested();

    private:
        void add_to_table(int row);
        void show_context_menu(const QPoint& position);

        scan::ScanEngine&    engine_;
        table::AddressTable& table_;

        QLabel*                    header_ {};
        widgets::StatusLabel*      note_ {};
        QTableView*                table_view_ {};
        models::FoundResultsModel* model_ {};
        QPushButton*               memory_view_button_ {};
        QPushButton*               add_address_button_ {};

        // The snapshot currently shown, so an unchanged poll does not reset the
        // model (a reset would drop the selection and re-run the sort).
        scan::ScanSnapshot last_snapshot_;
        scan::ScanConfig   last_config_;
        bool               has_last_ {false};
    };

} // namespace slopkit::ui::panels
