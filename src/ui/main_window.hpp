#pragma once

#include <functional>
#include <optional>

#include "plugin/plugin_host.hpp"
#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "table/address_table.hpp"
#include "ui/dialogs/table_conflict.hpp"

#include <QLabel>
#include <QMainWindow>
#include <QProgressBar>
#include <QString>
#include <QTimer>

class QAction;

namespace slopkit::ui::dialogs
{
    class AddAddressDialog;
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

namespace slopkit::ui
{
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
        void refresh_target_label();
        void run_freeze_pass();

        void on_memory_view_requested(quint64 address);
        void on_add_address_requested();
        void on_table_loaded();
        void show_process_list();
        void show_add_address();
        void show_table_settings();
        void show_log();
        void show_settings();
        void show_about();

        process::AccessWorker&   worker_;
        process::AttachedTarget& target_;
        plugin::PluginHost&      host_;
        SettingsController&      settings_;

        // The table path passed on the command line; empty when none.
        QString initial_table_path_;

        // The conflict prompt; the default runs the themed dialog.
        TableConflictPrompt table_conflict_prompt_ = [](QWidget* parent, const dialogs::TableConflictInfo& info)
        {
            return dialogs::ask_table_conflict(parent, info);
        };

        table::AddressTable address_table_;

        panels::ScannerPanel*     scanner_ {};
        panels::FoundListPanel*   found_list_ {};
        panels::AddressListPanel* address_list_ {};

        dialogs::ProcessListDialog*   process_list_ {};
        dialogs::AddAddressDialog*    add_address_ {};
        dialogs::TableSettingsDialog* table_settings_ {};
        dialogs::MemoryViewerDialog*  memory_view_ {};
        dialogs::LogDialog*           log_ {};
        dialogs::SettingsDialog*      settings_dialog_ {};

        QLabel*               process_label_ {};
        widgets::StatusLabel* address_status_ {};
        QProgressBar*         scan_progress_ {};

        QAction* quit_action_ {};
        QAction* open_process_action_ {};
        QAction* open_table_action_ {};
        QAction* save_table_action_ {};
        QAction* save_table_as_action_ {};
        QAction* log_action_ {};
        QAction* settings_action_ {};
        QAction* about_action_ {};

        QTimer* tick_timer_ {};

        // In-flight freeze job, if any; one at a time.
        std::optional<process::JobId> freeze_pending_;

        // The two stages of a table-driven auto attach: the process listing and
        // then the attach itself.
        std::optional<process::JobId> target_lookup_pending_;
        std::optional<process::JobId> auto_attach_pending_;
    };

} // namespace slopkit::ui
