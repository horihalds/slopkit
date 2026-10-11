#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "debug/breakpoints.hpp"
#include "process/types.hpp"
#include "scan/engine.hpp"
#include "table/address_table.hpp"
#include "ui/address_format.hpp"
#include "ui/live_values.hpp"

#include <QWidget>

class QAction;
class QLabel;
class QMenu;
class QPushButton;
class QTableView;

namespace slopkit::ui::models
{
    class FoundResultsModel;
    enum class CopyFormat;
}

namespace slopkit::ui::panels
{
    class FoundListPanel : public QWidget, public ui::LiveSurface
    {
        Q_OBJECT

    public:
        FoundListPanel(scan::ScanEngine& engine, table::AddressTable& table, QWidget* parent = nullptr);

        void refresh();

        void set_target_attached(bool attached);

        void set_modules(std::vector<process::ModuleInfo> modules);

        void set_address_mode(ui::AddressMode mode);

        void populate_row_menu(QMenu& menu, int row);

        [[nodiscard]] QWidget* tab_order_first() const noexcept;
        [[nodiscard]] QWidget* tab_order_last() const noexcept;

        [[nodiscard]] std::vector<ui::LiveRequest> next_live_request() override;
        void apply_live_readings(std::span<const ui::LiveReading> readings) override;

    signals:
        void memoryViewRequested();

        void accessWatchRequested(std::uint64_t address, std::size_t width, slopkit::debug::Kind kind);

        void browseRequested(quint64 address);

        void changeValuesRequested(std::vector<std::uint64_t> addresses, std::vector<std::byte> bytes);

    private:
        void add_to_table(int row);
        void copy_row(int row, models::CopyFormat format);
        void show_context_menu(const QPoint& position);

        void edit_value(int row);
        void browse_region(int row);
        void edit_selected_value();
        void browse_selected_region();

        scan::ScanEngine&    engine_;
        table::AddressTable& table_;

        QLabel*                    header_ {};
        QTableView*                table_view_ {};
        models::FoundResultsModel* model_ {};
        QPushButton*               memory_view_button_ {};

        QAction* edit_value_action_ {};
        QAction* browse_region_action_ {};

        scan::ScanSnapshot last_snapshot_;
        scan::ScanConfig   last_config_;
        bool               has_last_ {false};
        bool               target_attached_ {false};
    };

}
