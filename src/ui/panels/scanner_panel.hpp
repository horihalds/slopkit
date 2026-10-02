#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "process/access.hpp"
#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "scan/engine.hpp"
#include "scan/types.hpp"
#include "scan/value.hpp"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QProgressBar;
class QPushButton;

namespace slopkit::ui::widgets
{
    class CollapsibleSection;
    class PrimaryButton;
    class ScrollingComboBox;
    class StatusLabel;
} // namespace slopkit::ui::widgets

namespace slopkit::ui::panels
{

    // The right half of the middle zone: the scan controls. It builds the
    // ScanConfig, owns the engine and the worker session the engine reads
    // through, and never blocks on target access.
    class ScannerPanel : public QWidget
    {
        Q_OBJECT

    public:
        ScannerPanel(process::AccessWorker& worker, process::AttachedTarget& target, QWidget* parent = nullptr);

        [[nodiscard]] scan::ScanEngine& engine() noexcept;

        // Sets the fast-scan alignment field, used by the Settings dialog.
        void set_default_alignment(std::uint64_t alignment);

        // Polled by the window's tick: refreshes the enable state, the progress
        // bar and the status line.
        void refresh();

        [[nodiscard]] int progress_percent() const noexcept;

    signals:
        // A request to open the Memory Viewer at an address.
        void memoryViewRequested(quint64 address);

        // A request to open the Add Address dialog.
        void addAddressRequested();

    private:
        void build_layout();
        void connect_widgets();

        void request_scan_session();
        void request_memory_map();
        void apply_memory_map(process::MemoryMapResult result);
        void apply_range(std::uint64_t start, std::uint64_t end);
        void clear_memory_map();
        void rebuild_range_items();
        void on_range_selected(int index);
        void start_first_scan();
        void start_next_scan();
        void update_value_inputs();

        [[nodiscard]] scan::ScanType                               current_scan_type() const noexcept;
        [[nodiscard]] scan::ValueType                              current_value_type() const noexcept;
        [[nodiscard]] std::expected<scan::ScanConfig, std::string> build_config() const;

        process::AccessWorker&   worker_;
        process::AttachedTarget& target_;

        QLineEdit*                  value_edit_ {};
        QLineEdit*                  value_upper_edit_ {};
        QCheckBox*                  hex_check_ {};
        QComboBox*                  scan_type_combo_ {};
        QComboBox*                  value_type_combo_ {};
        widgets::ScrollingComboBox* module_combo_ {};
        QPushButton*                scan_button_ {};
        QPushButton*                next_scan_button_ {};
        QPushButton*                undo_button_ {};
        QPushButton*                cancel_button_ {};

        QLineEdit* start_edit_ {};
        QLineEdit* stop_edit_ {};
        QCheckBox* writable_check_ {};
        QCheckBox* executable_check_ {};
        QCheckBox* copy_on_write_check_ {};
        QCheckBox* fast_scan_check_ {};
        QLineEdit* alignment_edit_ {};
        QCheckBox* pause_scanning_check_ {};

        QPushButton* memory_view_button_ {};
        QPushButton* add_address_button_ {};

        QProgressBar*         scan_progress_ {};
        widgets::StatusLabel* status_label_ {};

        std::string status_;
        bool        status_is_error_ {false};

        int progress_percent_ {0};

        // A session separate from the app-wide one, used only by the scan
        // worker so the shared session is never touched from two threads. It is
        // declared before the engine so the engine joins its worker before the
        // session is destroyed. The session is created by an access-worker
        // handoff job and applied when that completion is drained.
        process::Session              worker_session_;
        process::ProcessId            worker_pid_ {0};
        std::string                   worker_plugin_;
        std::optional<process::JobId> handoff_pending_;

        // The target's memory map, fetched once per attach so the `All memory`
        // range spans the whole process address space: its lowest mapped page
        // up to the user-space ceiling. `map_pid_`/`map_plugin_` record which
        // target the pending or last request belongs to, so a completion for a
        // superseded target is dropped.
        std::optional<process::JobId>                          map_pending_;
        process::ProcessId                                     map_pid_ {0};
        std::string                                            map_plugin_;
        std::vector<process::ModuleInfo>                       modules_;
        std::optional<std::pair<std::uint64_t, std::uint64_t>> process_bounds_;
        bool                                                   range_updating_ {false};

        scan::ScanEngine engine_;
    };

} // namespace slopkit::ui::panels
