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
    class PrimaryButton;
    class ScrollingComboBox;
} // namespace slopkit::ui::widgets

namespace slopkit::ui::panels
{

    // The right half of the middle zone: the scan controls. It builds the
    // ScanConfig, owns the engine and the worker session the engine reads
    // through, and never blocks on target access. Its bottom-right button
    // requests the Add Address dialog, lined up with the found list's Memory
    // View button.
    class ScannerPanel : public QWidget
    {
        Q_OBJECT

    public:
        ScannerPanel(process::AccessWorker& worker, process::AttachedTarget& target, QWidget* parent = nullptr);

        ~ScannerPanel() override;

        [[nodiscard]] scan::ScanEngine& engine() noexcept;

        // Sets the fast-scan alignment field, used by the Settings dialog.
        void set_default_alignment(std::uint64_t alignment);

        // Focuses the scan value field and selects its whole content, so the
        // user can type a new value immediately; a no-op when the field is
        // disabled because the scan type needs no value.
        void focus_value_input();

        // Polled by the window's tick: refreshes the enable state and the
        // progress bar, and logs a changed engine message.
        void refresh();

        // Reports whether a debug session is running; while it is, pausing for a
        // scan is unavailable, because a SIGSTOP/SIGCONT pair would fight the
        // debugger.
        void set_debug_session_active(bool active);

        [[nodiscard]] int progress_percent() const noexcept;

        // Entry point of the target's main module (the flagged image, else the
        // lowest-based one), or its base when the plugin reports no entry; 0
        // before a memory map has been applied.
        [[nodiscard]] std::uint64_t main_module_address() const noexcept;

        // The seam the window joins its cross-panel chain through: the last
        // control of the scanner's own run and the footer pair. They exist only
        // so the window can splice the found list's controls between the pause
        // check and the footer buttons; the widgets themselves stay private.
        [[nodiscard]] QWidget* tab_order_last() const noexcept;         // Pause the game while scanning
        [[nodiscard]] QWidget* tab_order_footer_first() const noexcept; // Add Address Manually
        [[nodiscard]] QWidget* tab_order_footer_last() const noexcept;  // Table Settings

    signals:
        // A request to open the Add Address dialog, fed by the bottom button.
        void addAddressRequested();

        // A request to open the Table Settings dialog, fed by the bottom-right
        // button.
        void tableSettingsRequested();

        // The current memory map as file-backed module images, empty when
        // detached; the found list uses it to mark and group static hits.
        void memoryMapApplied(std::vector<process::ModuleInfo> modules);

    private:
        void build_layout();
        void connect_widgets();
        void apply_tab_order();

        void request_scan_session();
        void request_memory_map();
        void apply_memory_map(process::MemoryMapResult result);
        void apply_range(std::uint64_t start, std::uint64_t end);
        void clear_memory_map();
        void rebuild_range_items();
        void on_range_selected(int index);
        void start_first_scan();
        void start_next_scan();
        void activate_scan_from_input();
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
        QPushButton*                add_address_button_ {};
        QPushButton*                table_settings_button_ {};

        QLineEdit* start_edit_ {};
        QLineEdit* stop_edit_ {};
        QCheckBox* writable_check_ {};
        QCheckBox* executable_check_ {};
        QCheckBox* copy_on_write_check_ {};
        QCheckBox* fast_scan_check_ {};
        QLineEdit* alignment_edit_ {};
        QCheckBox* pause_scanning_check_ {};

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

        // Starts the engine, either directly or after a successful suspend of the
        // target; `refine` picks next_scan over first_scan.
        void start_engine(scan::ScanConfig config, bool refine);
        // Queues a scan behind a suspend when pausing is on and available, else
        // starts it immediately.
        void begin_scan(scan::ScanConfig config, bool refine);
        // Resumes a target this panel suspended once the engine has stopped; safe
        // to call every tick.
        void maybe_resume_target();
        // Enables the pause checkbox and picks its tooltip from the current
        // attach/capability/debug state.
        void update_pause_check();

        // Pause-while-scanning state machine, all touched on the UI thread: the
        // built config waiting for the in-flight suspend, the suspend/resume
        // jobs, and whether this panel currently holds the target stopped.
        struct PendingScan
        {
            scan::ScanConfig config;
            bool             refine {false};
        };

        std::optional<PendingScan>    pending_scan_;
        std::optional<process::JobId> suspend_pending_;
        std::optional<process::JobId> resume_pending_;
        bool                          target_suspended_ {false};
        bool                          resume_warn_logged_ {false};
        // The handoff reports whether the target's plugin can suspend, and the
        // window reports whether a debug session is running; both gate the check.
        bool                          can_suspend_ {false};
        bool                          debug_session_active_ {false};

        // The target's memory map, fetched once per attach so the `All memory`
        // range spans the whole process address space (0 to the user-space
        // ceiling) independently of the mapped pages. `map_pid_`/`map_plugin_`
        // record which target the pending or last request belongs to, so a
        // completion for a superseded target is dropped.
        std::optional<process::JobId>    map_pending_;
        process::ProcessId               map_pid_ {0};
        std::string                      map_plugin_;
        std::vector<process::ModuleInfo> modules_;
        bool                             map_ready_ {false};
        bool                             range_updating_ {false};

        scan::ScanEngine engine_;
    };

} // namespace slopkit::ui::panels
