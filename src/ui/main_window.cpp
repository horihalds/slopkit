#include "ui/main_window.hpp"

#include <chrono>
#include <format>
#include <string_view>
#include <utility>
#include <variant>

#include <QAction>
#include <QCoreApplication>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QSplitter>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "ui/components/widgets.hpp"
#include "ui/dialogs/add_address.hpp"
#include "ui/dialogs/log.hpp"
#include "ui/dialogs/memory_viewer.hpp"
#include "ui/dialogs/process_list.hpp"
#include "ui/dialogs/settings.hpp"
#include "ui/panels/address_list_panel.hpp"
#include "ui/panels/found_list_panel.hpp"
#include "ui/panels/scanner_panel.hpp"
#include "ui/settings.hpp"
#include "ui/theme.hpp"

namespace slopkit::ui
{

    namespace
    {
        // std::string_view -> QString for the status messages.
        QString to_qstring(std::string_view text)
        {
            return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
        }
    } // namespace

    MainWindow::MainWindow(process::AccessWorker&   worker,
                           process::AttachedTarget& target,
                           plugin::PluginHost&      host,
                           SettingsController&      settings,
                           QWidget*                 parent)
        : QMainWindow(parent), worker_(worker), target_(target), host_(host), settings_(settings)
    {
        setWindowTitle(QStringLiteral("slopkit"));
        setWindowIcon(widgets::application_icon());
        resize(748, 768);

        build_actions();
        build_menus();
        build_status_bar();
        build_central();
        build_dialogs();

        // Event-driven updates only: the tick polls the scan progress, applies a
        // missed drain and submits the freeze pass.
        tick_timer_ = new QTimer(this);
        connect(tick_timer_, &QTimer::timeout, this, &MainWindow::on_tick);
        tick_timer_->start(50);
    }

    MainWindow::~MainWindow() = default;

    void MainWindow::build_actions()
    {
        quit_action_ = new QAction(tr("Quit"), this);
        connect(quit_action_,
                &QAction::triggered,
                this,
                []
                {
                    log::debug(log::category::ui, "quit requested");
                    QCoreApplication::quit();
                });

        open_process_action_ = new QAction(tr("Open Process"), this);
        open_process_action_->setIcon(widgets::action_icon(widgets::ActionIcon::target));
        // No standard key exists for "pick a process".
        open_process_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));
        open_process_action_->setShortcutContext(Qt::WindowShortcut);

        open_table_action_ = new QAction(tr("Open Table"), this);
        open_table_action_->setIcon(widgets::action_icon(widgets::ActionIcon::open));
        open_table_action_->setShortcut(QKeySequence::Open);
        open_table_action_->setShortcutContext(Qt::WindowShortcut);

        save_table_action_ = new QAction(tr("Save Table"), this);
        save_table_action_->setIcon(widgets::action_icon(widgets::ActionIcon::save));
        // Shared by the File and Table menus; one action, so no shortcut ambiguity.
        save_table_action_->setShortcut(QKeySequence::Save);
        save_table_action_->setShortcutContext(Qt::WindowShortcut);

        save_table_as_action_ = new QAction(tr("Save Table As"), this);
        save_table_as_action_->setIcon(widgets::action_icon(widgets::ActionIcon::save));
        save_table_as_action_->setShortcut(QKeySequence::SaveAs);
        save_table_as_action_->setShortcutContext(Qt::WindowShortcut);

        log_action_ = new QAction(tr("Log"), this);

        settings_action_ = new QAction(tr("Settings"), this);

        about_action_ = new QAction(tr("About slopkit"), this);
    }

    void MainWindow::build_menus()
    {
        QMenu* file_menu = menuBar()->addMenu(tr("File"));
        file_menu->addAction(open_process_action_);
        file_menu->addAction(open_table_action_);
        file_menu->addAction(save_table_action_);
        file_menu->addAction(save_table_as_action_);
        file_menu->addSeparator();
        file_menu->addAction(quit_action_);

        QMenu* view_menu = menuBar()->addMenu(tr("View"));
        view_menu->addAction(log_action_);
        view_menu->addAction(settings_action_);

        QMenu* help_menu = menuBar()->addMenu(tr("Help"));
        help_menu->addAction(about_action_);
    }

    void MainWindow::build_status_bar()
    {
        process_label_ = new QLabel(to_qstring(target_.label()), this);
        statusBar()->addWidget(process_label_);
    }

    void MainWindow::build_central()
    {
        scanner_    = new panels::ScannerPanel(worker_, target_, this);
        found_list_ = new panels::FoundListPanel(scanner_->engine(), address_table_, this);

        connect(found_list_,
                &panels::FoundListPanel::memoryViewRequested,
                this,
                [this]
                {
                    on_memory_view_requested(scanner_->main_module_address());
                });
        connect(scanner_, &panels::ScannerPanel::addAddressRequested, this, &MainWindow::on_add_address_requested);
        connect(scanner_,
                &panels::ScannerPanel::memoryMapApplied,
                this,
                [this](std::vector<process::ModuleInfo> modules)
                {
                    log::debug(log::category::ui, std::format("memory map applied: {} module(s)", modules.size()));
                    found_list_->set_modules(modules);
                    address_list_->set_modules(modules);
                    memory_view_->set_modules(std::move(modules));
                });

        address_list_ = new panels::AddressListPanel(address_table_, worker_, target_, this);
        connect(address_list_, &panels::AddressListPanel::browseRequested, this, &MainWindow::on_memory_view_requested);
        connect(open_table_action_, &QAction::triggered, address_list_, &panels::AddressListPanel::open_table);
        connect(save_table_action_, &QAction::triggered, address_list_, &panels::AddressListPanel::save_table);
        connect(save_table_as_action_, &QAction::triggered, address_list_, &panels::AddressListPanel::save_table_as);

        auto* middle_splitter = new QSplitter(Qt::Horizontal, this);
        middle_splitter->addWidget(found_list_);
        middle_splitter->addWidget(scanner_);
        middle_splitter->setStretchFactor(0, 1);
        middle_splitter->setStretchFactor(1, 1);

        auto* vertical_splitter = new QSplitter(Qt::Vertical, this);
        vertical_splitter->addWidget(middle_splitter);
        vertical_splitter->addWidget(address_list_);
        // The scan zone keeps the scanner's content height, so the hits table's
        // bottom edge stays level with the Memory Scan Options panel; the address
        // list takes every remaining pixel of window height.
        vertical_splitter->setStretchFactor(0, 0);
        vertical_splitter->setStretchFactor(1, 1);

        scan_progress_ = new QProgressBar(this);
        scan_progress_->setRange(0, 100);
        scan_progress_->setValue(0);
        scan_progress_->setFormat(QStringLiteral("%p%"));

        // The scan progress spans the width of the window above the split zones.
        auto* column        = new QWidget(this);
        auto* column_layout = new QVBoxLayout(column);
        column_layout->setContentsMargins(0, 0, 0, 0);
        column_layout->addWidget(scan_progress_);
        column_layout->addWidget(vertical_splitter, 1);
        setCentralWidget(column);
    }

    void MainWindow::build_dialogs()
    {
        process_list_ = new dialogs::ProcessListDialog(worker_, target_, this);
        connect(process_list_,
                &dialogs::ProcessListDialog::targetChanged,
                this,
                [this]
                {
                    log::info(log::category::ui, std::format("target changed to {}", target_.label()));
                    refresh_target_label();
                });

        add_address_ = new dialogs::AddAddressDialog(address_table_, this);

        memory_view_ = new dialogs::MemoryViewerDialog(worker_, target_, this);

        log_ = new dialogs::LogDialog(this);

        // The dialog is a view over the shared controller; the window is the only
        // component that applies the persisted values to the live views.
        settings_dialog_ = new dialogs::SettingsDialog(host_, scanner_->engine(), settings_, this);
        connect(settings_dialog_,
                &dialogs::SettingsDialog::alignmentChanged,
                this,
                [this](quint64 alignment)
                {
                    scanner_->set_default_alignment(alignment);
                });

        connect(&settings_,
                &SettingsController::darkThemeChanged,
                this,
                [](bool dark)
                {
                    apply_theme(dark ? dark_theme() : light_theme());
                });
        connect(&settings_,
                &SettingsController::addressModeChanged,
                this,
                [this](ui::AddressMode mode)
                {
                    found_list_->set_address_mode(mode);
                    address_list_->set_address_mode(mode);
                    memory_view_->set_address_mode(mode);
                });

        // Apply the mode restored from disk before the window is first shown.
        const ui::AddressMode persisted_mode = settings_.values().address_mode;
        found_list_->set_address_mode(persisted_mode);
        address_list_->set_address_mode(persisted_mode);
        memory_view_->set_address_mode(persisted_mode);

        connect(open_process_action_, &QAction::triggered, this, &MainWindow::show_process_list);
        connect(log_action_, &QAction::triggered, this, &MainWindow::show_log);
        connect(settings_action_, &QAction::triggered, this, &MainWindow::show_settings);
        connect(about_action_, &QAction::triggered, this, &MainWindow::show_about);
    }

    void MainWindow::show_process_list()
    {
        log::debug(log::category::ui, "opening Process List dialog");
        process_list_->show();
        process_list_->raise();
        process_list_->activateWindow();
    }

    void MainWindow::show_add_address()
    {
        log::debug(log::category::ui, "opening Add Address dialog");
        add_address_->show();
        add_address_->raise();
        add_address_->activateWindow();
    }

    void MainWindow::show_log()
    {
        log::debug(log::category::ui, "opening Log dialog");
        log_->show();
        log_->raise();
        log_->activateWindow();
    }

    void MainWindow::show_settings()
    {
        log::debug(log::category::ui, "opening Settings dialog");
        settings_dialog_->show();
        settings_dialog_->raise();
        settings_dialog_->activateWindow();
    }

    void MainWindow::show_about()
    {
        settings_dialog_->select_about();
        show_settings();
    }

    void MainWindow::on_tick()
    {
        // The completion hook already drains on its own; this is the safety net
        // for a wake-up that raced with a busy UI.
        worker_.drain();
        scanner_->refresh();
        found_list_->refresh();
        address_list_->refresh();
        scan_progress_->setValue(scanner_->progress_percent());
        run_freeze_pass();
        refresh_target_label();
    }

    void MainWindow::on_memory_view_requested(quint64 address)
    {
        log::debug(log::category::ui, std::format("opening Memory Viewer at 0x{:X}", address));
        memory_view_->set_address(address);
        memory_view_->show();
        memory_view_->raise();
        memory_view_->activateWindow();
    }

    void MainWindow::on_add_address_requested()
    {
        show_add_address();
    }

    void MainWindow::refresh_target_label()
    {
        process_label_->setText(to_qstring(target_.label()));
        found_list_->set_target_attached(target_.valid());
    }

    void MainWindow::run_freeze_pass()
    {
        if (!target_.valid() || freeze_pending_.has_value())
        {
            return;
        }

        const auto now_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        auto items = address_table_.freeze_items(now_seconds);
        if (items.empty())
        {
            return;
        }

        const process::JobId job_id = worker_.next_job_id();
        freeze_pending_             = job_id;
        log::debug(log::category::ui, std::format("freeze pass submitted for {} item(s)", items.size()));
        const bool submitted = worker_.submit_freeze(
            job_id,
            std::move(items),
            [this, job_id](process::JobResult&& result)
            {
                if (freeze_pending_ != job_id)
                {
                    return;
                }
                freeze_pending_.reset();
                const auto& frozen = std::get<process::FreezeResult>(result);
                if (frozen.error)
                {
                    log::warning(log::category::ui,
                                 std::format("freeze pass failed: {}", process::describe(*frozen.error)));
                    address_list_->report_freeze_error(process::describe(*frozen.error));
                }
            });
        if (!submitted)
        {
            freeze_pending_.reset();
        }
    }

} // namespace slopkit::ui
