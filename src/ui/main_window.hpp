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
    class MemoryViewerDialog;
    class ProcessListDialog;
    class SettingsDialog;
} // namespace slopkit::ui::dialogs

namespace slopkit::ui::panels
{
    class AddressListPanel;
    class FoundListPanel;
    class ScannerPanel;
} // namespace slopkit::ui::panels

namespace slopkit::ui
{

    // The application window: menu bar, toolbar, status bar and the three split
    // zones. It owns the address table and forwards every target access to the
    // shared AccessWorker.
    class MainWindow : public QMainWindow
    {
        Q_OBJECT

    public:
        MainWindow(process::AccessWorker&   worker,
                   process::AttachedTarget& target,
                   plugin::PluginHost&      host,
                   QWidget*                 parent = nullptr);
        ~MainWindow() override;

    private slots:
        void on_tick();

    private:
        void build_actions();
        void build_menus();
        void build_toolbar();
        void build_status_bar();
        void build_central();
        void build_dialogs();
        void refresh_target_label();
        void run_freeze_pass();

        void on_memory_view_requested(quint64 address);
        void on_add_address_requested();
        void show_process_list();
        void show_add_address();
        void show_settings();
        void show_about();

        process::AccessWorker&   worker_;
        process::AttachedTarget& target_;
        plugin::PluginHost&      host_;

        table::AddressTable address_table_;

        panels::ScannerPanel*     scanner_ {};
        panels::FoundListPanel*   found_list_ {};
        panels::AddressListPanel* address_list_ {};

        dialogs::ProcessListDialog*  process_list_ {};
        dialogs::AddAddressDialog*   add_address_ {};
        dialogs::MemoryViewerDialog* memory_view_ {};
        dialogs::SettingsDialog*     settings_ {};

        // The theme currently installed, mirrored by the Settings dialog.
        bool dark_theme_active_ {true};

        QLabel*       process_label_ {};
        QProgressBar* scan_progress_ {};

        QAction* quit_action_ {};
        QAction* open_process_action_ {};
        QAction* open_table_action_ {};
        QAction* save_table_action_ {};
        QAction* undo_scan_action_ {};
        QAction* add_address_action_ {};
        QAction* settings_action_ {};
        QAction* about_action_ {};
        QAction* delete_selected_action_ {};
        QAction* freeze_selected_action_ {};

        QTimer* tick_timer_ {};

        // In-flight freeze job, if any; one at a time.
        std::optional<process::JobId> freeze_pending_;
    };

} // namespace slopkit::ui
