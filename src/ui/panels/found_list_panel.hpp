#pragma once

#include <vector>

#include "process/types.hpp"
#include "scan/engine.hpp"
#include "table/address_table.hpp"
#include "ui/address_format.hpp"

#include <QWidget>

class QLabel;
class QPushButton;
class QTableView;

namespace slopkit::ui::models
{
    class FoundResultsModel;
} // namespace slopkit::ui::models

namespace slopkit::ui::panels
{

    // The left half of the middle zone: the `Showing N of M results` list. It
    // only reads the engine's snapshot and appends double-clicked hits to the
    // address table. Its entry row hosts the Memory View button.
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

        // Announces a fresh module map; the found list marks and groups static
        // hits. Wired from the scanner panel by the window.
        void set_modules(std::vector<process::ModuleInfo> modules);

        // Chooses how static addresses are shown in the Address column.
        void set_address_mode(ui::AddressMode mode);

    signals:
        // A request to open the Memory Viewer at the main module's entry point
        // (or its base when the plugin reports none).
        void memoryViewRequested();

    private:
        void add_to_table(int row);
        void show_context_menu(const QPoint& position);

        scan::ScanEngine&    engine_;
        table::AddressTable& table_;

        QLabel*                    header_ {};
        QTableView*                table_view_ {};
        models::FoundResultsModel* model_ {};
        QPushButton*               memory_view_button_ {};

        // The snapshot currently shown, so an unchanged poll does not reset the
        // model (a reset would drop the selection and re-run the sort).
        scan::ScanSnapshot last_snapshot_;
        scan::ScanConfig   last_config_;
        bool               has_last_ {false};
    };

} // namespace slopkit::ui::panels
