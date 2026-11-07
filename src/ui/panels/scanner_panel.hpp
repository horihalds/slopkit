#pragma once

#include <chrono>
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
}

namespace slopkit::ui::panels
{
    class ScannerPanel : public QWidget
    {
        Q_OBJECT

    public:
        ScannerPanel(process::AccessWorker& worker, process::AttachedTarget& target, QWidget* parent = nullptr);

        ~ScannerPanel() override;

        [[nodiscard]] scan::ScanEngine& engine() noexcept;

        void set_default_alignment(std::uint64_t alignment);

        void focus_value_input();

        // Primes the value field for a search started elsewhere: the text fills
        // the value box, the byte count picks the value type and the focus lands
        // on the box so the user can start the scan.
        void prime_value(const QString& text, int bytes);

        void refresh();

        // Reports whether a debug session is running; while it is, pausing for a
        // scan is unavailable, because a SIGSTOP/SIGCONT pair would fight the
        // debugger.
        void set_debug_session_active(bool active);

        [[nodiscard]] int progress_percent() const noexcept;

        [[nodiscard]] std::uint64_t main_module_address() const noexcept;

        [[nodiscard]] QWidget* tab_order_last() const noexcept;

    signals:
        void memoryMapApplied(std::vector<process::ModuleInfo> modules, std::vector<process::RegionInfo> regions);

    private:
        void build_layout();
        void connect_widgets();
        void apply_tab_order();

        void request_scan_session();
        void request_memory_map();
        void apply_memory_map(process::MemoryMapResult result);
        void note_memory_map_failure(process::AccessError error);
        void apply_range(std::uint64_t start, std::uint64_t end);
        void clear_memory_map();
        void rebuild_range_items();
        void on_range_selected(int index);
        void start_first_scan();
        void start_next_scan();
        void activate_scan_from_input();
        void update_value_inputs();

        void apply_default_alignment();

        void convert_value_base_fields(bool to_hex);

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

        QLineEdit*      start_edit_ {};
        QLineEdit*      stop_edit_ {};
        QCheckBox*      writable_check_ {};
        QCheckBox*      executable_check_ {};
        QCheckBox*      copy_on_write_check_ {};
        QCheckBox*      fast_scan_check_ {};
        QLineEdit*      alignment_edit_ {};
        scan::ValueType alignment_value_type_ {scan::ValueType::int32};
        QCheckBox*      pause_scanning_check_ {};

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

        void start_engine(scan::ScanConfig config, bool refine);
        void begin_scan(scan::ScanConfig config, bool refine);
        void maybe_resume_target();
        void update_pause_check();

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
        bool                          can_suspend_ {false};
        bool                          debug_session_active_ {false};

        // The memory map is re-requested until it succeeds, so a single failed
        // fetch (for example while the Wine resident agent is being brought up)
        // no longer strips module+RVA from the whole session. `map_pid_`/
        // `map_plugin_` track the target the map belongs to, `map_ready_` says
        // whether it has been applied, and `map_retry_after_` rate-limits the
        // retries.
        std::optional<process::JobId>         map_pending_;
        process::ProcessId                    map_pid_ {0};
        std::string                           map_plugin_;
        std::vector<process::ModuleInfo>      modules_;
        bool                                  map_ready_ {false};
        unsigned                              map_attempts_ {0};
        std::chrono::steady_clock::time_point map_retry_after_ {};
        bool                                  map_failure_logged_ {false};
        bool                                  range_updating_ {false};

        scan::ScanEngine engine_;
    };

}
