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
#include "script/symbols.hpp"
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
    class ReClassDialog;
    class SettingsDialog;
    class TableSettingsDialog;
}

namespace slopkit::ui::panels
{
    class AddressListPanel;
    class CompanionBar;
    class FoundListPanel;
    class ScannerPanel;
}

namespace slopkit::ui::widgets
{
    class StatusLabel;
}

namespace slopkit::ui::dialogs
{
    class BreakpointsDialog;
}

namespace slopkit::debug
{
    class Controller;
}

namespace slopkit::ui
{
    class LiveValues;
    class ScriptUpdates;
    class SettingsController;

    class MainWindow : public QMainWindow
    {
        Q_OBJECT

    public:
        MainWindow(process::AccessWorker&   worker,
                   process::AttachedTarget& target,
                   plugin::PluginHost&      host,
                   SettingsController&      settings,
                   debug::Controller&       debug,
                   script::SymbolTable&     symbols            = script::default_symbol_table(),
                   const QString&           initial_table_path = QString(),
                   QWidget*                 parent             = nullptr);
        ~MainWindow() override;

        using TableConflictPrompt =
            std::function<dialogs::TableConflictChoice(QWidget* parent, const dialogs::TableConflictInfo& info)>;

        [[nodiscard]] bool open_table_request(const QString& path);

        void set_table_conflict_prompt(TableConflictPrompt prompt);

        using SandboxLauncher     = std::function<std::expected<std::int64_t, std::string>()>;
        using SandboxAttachPrompt = std::function<bool(QWidget* parent, process::ProcessId pid)>;
        using SandboxTerminator   = std::function<std::expected<void, std::string>(std::int64_t pid)>;

        void set_sandbox_launcher(SandboxLauncher launcher);
        void set_sandbox_attach_prompt(SandboxAttachPrompt prompt);
        void set_sandbox_terminator(SandboxTerminator terminator);

        // The detached Memory Viewer, or nullptr before build_dialogs(); it is a
        // top-level window with no parent widget, so the compositor stacks it
        // like any other window instead of keeping it above this one.
        [[nodiscard]] dialogs::MemoryViewerDialog* memory_viewer() const noexcept;

        // The detached Log window, or nullptr before build_dialogs(); like the
        // Memory Viewer it is a parentless top-level window.
        [[nodiscard]] dialogs::LogDialog* log() const noexcept;

        // The detached Breakpoints and Access Watch windows, or nullptr before
        // build_dialogs(); like the Memory Viewer they are parentless top-level
        // windows.
        [[nodiscard]] dialogs::BreakpointsDialog* breakpoints() const noexcept;
        [[nodiscard]] dialogs::AccessWatchDialog* access_watch() const noexcept;

        // The detached ReClass window, or nullptr before build_dialogs().
        [[nodiscard]] dialogs::ReClassDialog* reclass() const noexcept;

    protected:
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

        void on_found_values_requested(std::vector<std::uint64_t> addresses, std::vector<std::byte> bytes);
        void on_add_address_requested();
        void on_edit_entry_requested(std::size_t row);
        void on_table_loaded();
        void start_sandbox_for_table();
        void stop_owned_sandbox();

        using ProcessMatcher =
            std::function<const process::ProcessInfo*(std::span<const process::ProcessInfo> processes)>;

        void lookup_and_attach(ProcessMatcher match, std::string_view context, std::string missing);

        void submit_process_attach(const process::ProcessInfo& info, std::string_view context);

        void show_process_list();
        void show_add_address();
        void show_table_settings();
        void on_launch_sandbox_requested();
        void show_log();
        void show_breakpoints();
        void show_access_watch();
        void show_reclass();
        void show_settings();
        void show_about();
        void show_generate_trainer();

        void start_access_watch(std::uint64_t address, std::size_t width, debug::Kind kind);

        // Primes the scanner with a value another surface asked to search for
        // (the ReClass window's node menu) and leaves the focus on the value
        // field so the user can start the scan.
        void search_for_value(const QString& text, int bytes);

        void show_instruction_accesses(std::uint64_t                   instruction,
                                       std::size_t                     instruction_length,
                                       std::vector<ui::ResolvedAccess> accesses);

        process::AccessWorker&   worker_;
        process::AttachedTarget& target_;
        plugin::PluginHost&      host_;
        SettingsController&      settings_;
        debug::Controller&       debug_;
        script::SymbolTable&     symbols_;

        QString initial_table_path_;

        TableConflictPrompt table_conflict_prompt_ = [](QWidget* parent, const dialogs::TableConflictInfo& info)
        {
            return dialogs::ask_table_conflict(parent, info);
        };

        SandboxLauncher     sandbox_launcher_;
        SandboxAttachPrompt sandbox_attach_prompt_;
        SandboxTerminator   sandbox_terminator_;

        // The practice target this window launched for a table, if any; only it
        // is stopped on a clean shutdown.
        std::optional<std::int64_t> owned_sandbox_pid_;

        table::AddressTable address_table_;

        panels::ScannerPanel*     scanner_ {};
        panels::CompanionBar*     companion_bar_ {};
        panels::FoundListPanel*   found_list_ {};
        panels::AddressListPanel* address_list_ {};

        dialogs::ProcessListDialog*                  process_list_ {};
        dialogs::AddAddressDialog*                   add_address_ {};
        dialogs::TableSettingsDialog*                table_settings_ {};
        std::unique_ptr<dialogs::MemoryViewerDialog> memory_view_;
        std::unique_ptr<dialogs::LogDialog>          log_;
        std::unique_ptr<dialogs::BreakpointsDialog>  breakpoints_;
        std::unique_ptr<dialogs::AccessWatchDialog>  access_watch_;
        std::unique_ptr<dialogs::ReClassDialog>      reclass_;
        dialogs::SettingsDialog*                     settings_dialog_ {};

        QLabel*               process_label_ {};
        widgets::StatusLabel* address_status_ {};
        QProgressBar*         scan_progress_ {};

        QAction* quit_action_ {};
        QAction* open_process_action_ {};
        QAction* open_table_action_ {};
        QAction* save_table_action_ {};
        QAction* save_table_as_action_ {};
        QAction* generate_trainer_action_ {};
        QAction* add_script_action_ {};
        QAction* log_action_ {};
        QAction* settings_action_ {};
        QAction* breakpoints_action_ {};
        QAction* access_watch_action_ {};
        QAction* reclass_action_ {};
        QAction* launch_sandbox_action_ {};
        QAction* about_action_ {};

        QTimer* tick_timer_ {};

        LiveValues* live_values_ {};

        std::unique_ptr<ScriptUpdates> script_updates_;

        std::optional<process::JobId> freeze_pending_;

        std::optional<process::JobId> found_write_pending_;

        // The two stages of a listing-driven attach: the process listing and
        // then the attach itself. A table load and a practice-target launch
        // share them, so only one attach runs at a time.
        std::optional<process::JobId> target_lookup_pending_;
        std::optional<process::JobId> auto_attach_pending_;
    };

}
