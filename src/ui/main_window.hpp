#pragma once

#include <optional>

#include "plugin/plugin_host.hpp"
#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "table/address_table.hpp"

#include <QLabel>
#include <QMainWindow>
#include <QProgressBar>
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
                   QWidget*                 parent = nullptr);
        ~MainWindow() override;

    private slots:
        void on_tick();

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

        QLabel*       process_label_ {};
        QProgressBar* scan_progress_ {};

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
