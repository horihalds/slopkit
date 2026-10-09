#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "debug/breakpoints.hpp"
#include "plugin/plugin_host.hpp"
#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "table/address_table.hpp"
#include "ui/access_watch.hpp"
#include "ui/dialogs/table_conflict.hpp"

#include <QLabel>
#include <QMainWindow>
#include <QProgressBar>
#include <QString>
#include <QTimer>

class QAction;
class QEvent;

namespace slopkit::ui::dialogs
{
    class AddAddressDialog;
    class AccessWatchDialog;
    class LogDialog;
    class MemoryViewerDialog;
    class ProcessListDialog;
    class SettingsDialog;
    class TableSettingsDialog;
} // namespace slopkit::ui::dialogs

namespace slopkit::ui::panels
{
    class AddressListPanel;
    class FoundListPanel;
    class ScannerPanel;
} // namespace slopkit::ui::panels

namespace slopkit::ui::widgets
{
    class StatusLabel;
} // namespace slopkit::ui::widgets

namespace slopkit::ui::dialogs
{
    class BreakpointsDialog;
} // namespace slopkit::ui::dialogs

namespace slopkit::debug
{
    class Controller;
} // namespace slopkit::debug

namespace slopkit::ui
{
    class LiveValues;
    class SettingsController;

    // The application window: menu bar, status bar and the three split zones. It
    // owns the address table and forwards every target access to the shared
    // AccessWorker.
    class MainWindow : public QMainWindow
    {
        Q_OBJECT

    public:
        MainWindow(process::AccessWorker&   worker,
                   process::AttachedTarget& target,
                   plugin::PluginHost&      host,
                   SettingsController&      settings,
                   debug::Controller&       debug,
                   const QString&           initial_table_path = QString(),
                   QWidget*                 parent             = nullptr);
        ~MainWindow() override;

        // The conflict prompt the open flow uses by default: the themed
        // Cancel/Overwrite/Merge dialog.
        using TableConflictPrompt =
            std::function<dialogs::TableConflictChoice(QWidget* parent, const dialogs::TableConflictInfo& info)>;

        // Opens `path` as the address table: an empty table is replaced at once,
        // a non-empty one goes through the conflict prompt. Returns false when
        // the file could not be parsed or the user cancelled.
        [[nodiscard]] bool open_table_request(const QString& path);

        // Replaces the prompt the open flow uses, so a test can answer without a
        // dialog.
        void set_table_conflict_prompt(TableConflictPrompt prompt);

        // The launch and the attach question of Help > Launch Practice Target;
        // production uses app::launch_sandbox and the themed Yes/No prompt.
        using SandboxLauncher     = std::function<std::expected<std::int64_t, std::string>()>;
        using SandboxAttachPrompt = std::function<bool(QWidget* parent, process::ProcessId pid)>;

        // Replace the launch and the attach question, so a test can drive the
        // flow without spawning a process or showing a dialog.
        void set_sandbox_launcher(SandboxLauncher launcher);
        void set_sandbox_attach_prompt(SandboxAttachPrompt prompt);

        // The detached Memory Viewer, or nullptr before build_dialogs(); it is a
        // top-level window with no parent widget, so the compositor stacks it
        // like any other window instead of keeping it above this one.
        [[nodiscard]] dialogs::MemoryViewerDialog* memory_viewer() const noexcept;

    protected:
        // Focuses the scanner's value box whenever the window becomes active
        // again, so alt-tabbing back leaves the keyboard ready for a new value.
        bool event(QEvent* event) override;

    private slots:
        void on_tick();
        void remember_table_path(const QString& path);
        void remember_file_path(const QString& path);
        void on_open_table_requested();

    private:
        void build_actions();
        void build_menus();
        void build_status_bar();
        void build_central();
        void build_dialogs();
        void apply_tab_order();
        void refresh_target_label();
        void run_freeze_pass();

        void on_memory_view_requested(quint64 address);
        void on_add_address_requested();
        void on_table_loaded();

        // A predicate that picks the target out of one process listing.
        using ProcessMatcher =
            std::function<const process::ProcessInfo*(std::span<const process::ProcessInfo> processes)>;

        // Runs one listing and attaches to the process `match` picks. `context`
        // ("Table auto", "Practice target") prefixes the status lines and
        // `missing` is reported when nothing matches. Shared by the table auto
        // attach and the practice-target launch.
        void lookup_and_attach(ProcessMatcher match, std::string_view context, std::string missing);

        // Submits the app attach for a process found in a listing and applies the
        // result to the shared target.
        void submit_process_attach(const process::ProcessInfo& info, std::string_view context);

        void show_process_list();
        void show_add_address();
        void show_table_settings();
        void on_launch_sandbox_requested();
        void show_log();
        void show_breakpoints();
        void show_access_watch();
        void show_settings();
        void show_about();

        // The three context-menu/resolved commands funnel into the Access Watch
        // window: arm a watch, or show the resolved operands of a listing row.
        void start_access_watch(std::uint64_t address, std::size_t width, debug::Kind kind);
        void show_instruction_accesses(std::uint64_t                   instruction,
                                       std::size_t                     instruction_length,
                                       std::vector<ui::ResolvedAccess> accesses);

        process::AccessWorker&   worker_;
        process::AttachedTarget& target_;
        plugin::PluginHost&      host_;
        SettingsController&      settings_;
        debug::Controller&       debug_;

        // The table path passed on the command line; empty when none.
        QString initial_table_path_;

        // The conflict prompt; the default runs the themed dialog.
        TableConflictPrompt table_conflict_prompt_ = [](QWidget* parent, const dialogs::TableConflictInfo& info)
        {
            return dialogs::ask_table_conflict(parent, info);
        };

        // The practice-target launch and its attach question; the constructor
        // installs the production values.
        SandboxLauncher     sandbox_launcher_;
        SandboxAttachPrompt sandbox_attach_prompt_;

        table::AddressTable address_table_;

        panels::ScannerPanel*     scanner_ {};
        panels::FoundListPanel*   found_list_ {};
        panels::AddressListPanel* address_list_ {};

        dialogs::ProcessListDialog*                  process_list_ {};
        dialogs::AddAddressDialog*                   add_address_ {};
        dialogs::TableSettingsDialog*                table_settings_ {};
        std::unique_ptr<dialogs::MemoryViewerDialog> memory_view_;
        dialogs::LogDialog*                          log_ {};
        dialogs::BreakpointsDialog*                  breakpoints_ {};
        dialogs::AccessWatchDialog*                  access_watch_ {};
        dialogs::SettingsDialog*                     settings_dialog_ {};

        QLabel*               process_label_ {};
        widgets::StatusLabel* address_status_ {};
        QProgressBar*         scan_progress_ {};

        QAction* quit_action_ {};
        QAction* open_process_action_ {};
        QAction* open_table_action_ {};
        QAction* save_table_action_ {};
        QAction* save_table_as_action_ {};
        QAction* add_script_action_ {};
        QAction* log_action_ {};
        QAction* settings_action_ {};
        QAction* breakpoints_action_ {};
        QAction* access_watch_action_ {};
        QAction* launch_sandbox_action_ {};
        QAction* about_action_ {};

        QTimer* tick_timer_ {};

        // The one live-pass coordinator; owns the cadence the three surfaces
        // follow and submits a single batched read per interval.
        LiveValues* live_values_ {};

        // In-flight freeze job, if any; one at a time.
        std::optional<process::JobId> freeze_pending_;

        // The two stages of a listing-driven attach: the process listing and
        // then the attach itself. A table load and a practice-target launch
        // share them, so only one attach runs at a time.
        std::optional<process::JobId> target_lookup_pending_;
        std::optional<process::JobId> auto_attach_pending_;
    };

} // namespace slopkit::ui
