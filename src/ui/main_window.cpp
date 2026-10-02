#include "ui/main_window.hpp"

#include <chrono>
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

#include "ui/components/widgets.hpp"
#include "ui/dialogs/add_address.hpp"
#include "ui/dialogs/memory_viewer.hpp"
#include "ui/dialogs/process_list.hpp"
#include "ui/dialogs/settings.hpp"
#include "ui/panels/address_list_panel.hpp"
#include "ui/panels/found_list_panel.hpp"
#include "ui/panels/scanner_panel.hpp"
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
                           QWidget*                 parent)
        : QMainWindow(parent), worker_(worker), target_(target), host_(host)
    {
        setWindowTitle(QStringLiteral("slopkit"));
        setWindowIcon(widgets::application_icon());
        resize(1100, 720);

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
                    QCoreApplication::quit();
                });

        open_process_action_ = new QAction(tr("Open Process..."), this);
        // No standard key exists for "pick a process".
        open_process_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));
        open_process_action_->setShortcutContext(Qt::WindowShortcut);

        open_table_action_ = new QAction(tr("Open Table..."), this);
        open_table_action_->setShortcut(QKeySequence::Open);
        open_table_action_->setShortcutContext(Qt::WindowShortcut);

        save_table_action_ = new QAction(tr("Save Table..."), this);
        // Shared by the File and Table menus; one action, so no shortcut ambiguity.
        save_table_action_->setShortcut(QKeySequence::Save);
        save_table_action_->setShortcutContext(Qt::WindowShortcut);

        undo_scan_action_ = new QAction(tr("Undo Scan"), this);

        add_address_action_ = new QAction(tr("Add Address Manually..."), this);

        settings_action_ = new QAction(tr("Settings..."), this);

        about_action_ = new QAction(tr("About slopkit"), this);

        delete_selected_action_ = new QAction(tr("Delete Selected"), this);

        freeze_selected_action_ = new QAction(tr("Freeze Selected"), this);
    }

    void MainWindow::build_menus()
    {
        QMenu* file_menu = menuBar()->addMenu(tr("File"));
        file_menu->addAction(open_process_action_);
        file_menu->addAction(open_table_action_);
        file_menu->addAction(save_table_action_);
        file_menu->addSeparator();
        file_menu->addAction(quit_action_);

        QMenu* edit_menu = menuBar()->addMenu(tr("Edit"));
        edit_menu->addAction(undo_scan_action_);
        edit_menu->addAction(add_address_action_);
        edit_menu->addSeparator();
        edit_menu->addAction(settings_action_);

        QMenu* table_menu = menuBar()->addMenu(tr("Table"));
        table_menu->addAction(delete_selected_action_);
        table_menu->addAction(freeze_selected_action_);
        table_menu->addSeparator();
        table_menu->addAction(save_table_action_);

        QMenu* d3d_menu    = menuBar()->addMenu(tr("D3D"));
        auto*  placeholder = new QAction(tr("Placeholder"), this);
        placeholder->setEnabled(false);
        d3d_menu->addAction(placeholder);

        QMenu* help_menu = menuBar()->addMenu(tr("Help"));
        help_menu->addAction(about_action_);
    }

    void MainWindow::build_status_bar()
    {
        process_label_ = new QLabel(to_qstring(target_.label()), this);
        statusBar()->addWidget(process_label_);

        scan_progress_ = new QProgressBar(this);
        scan_progress_->setRange(0, 100);
        scan_progress_->setValue(0);
        scan_progress_->setFormat(QStringLiteral("%p%"));
        statusBar()->addPermanentWidget(scan_progress_);
    }

    void MainWindow::build_central()
    {
        scanner_    = new panels::ScannerPanel(worker_, target_, this);
        found_list_ = new panels::FoundListPanel(scanner_->engine(), address_table_, this);

        connect(scanner_, &panels::ScannerPanel::memoryViewRequested, this, &MainWindow::on_memory_view_requested);
        connect(scanner_, &panels::ScannerPanel::addAddressRequested, this, &MainWindow::on_add_address_requested);
        connect(undo_scan_action_,
                &QAction::triggered,
                this,
                [this]
                {
                    scanner_->engine().undo();
                    scanner_->refresh();
                });

        address_list_ = new panels::AddressListPanel(address_table_, worker_, target_, this);
        connect(address_list_, &panels::AddressListPanel::browseRequested, this, &MainWindow::on_memory_view_requested);
        connect(open_table_action_, &QAction::triggered, address_list_, &panels::AddressListPanel::open_table);
        connect(save_table_action_, &QAction::triggered, address_list_, &panels::AddressListPanel::save_table);
        connect(
            delete_selected_action_, &QAction::triggered, address_list_, &panels::AddressListPanel::delete_selected);
        connect(freeze_selected_action_,
                &QAction::triggered,
                address_list_,
                &panels::AddressListPanel::toggle_freeze_selected);

        auto* middle_splitter = new QSplitter(Qt::Horizontal, this);
        middle_splitter->addWidget(found_list_);
        middle_splitter->addWidget(scanner_);
        middle_splitter->setStretchFactor(0, 1);
        middle_splitter->setStretchFactor(1, 1);

        auto* vertical_splitter = new QSplitter(Qt::Vertical, this);
        vertical_splitter->addWidget(middle_splitter);
        vertical_splitter->addWidget(address_list_);
        // 62 % of the height goes to the middle scan zone, 38 % to the list.
        vertical_splitter->setStretchFactor(0, 62);
        vertical_splitter->setStretchFactor(1, 38);

        setCentralWidget(vertical_splitter);
    }

    void MainWindow::build_dialogs()
    {
        process_list_ = new dialogs::ProcessListDialog(worker_, target_, this);
        connect(process_list_, &dialogs::ProcessListDialog::targetChanged, this, &MainWindow::refresh_target_label);

        add_address_ = new dialogs::AddAddressDialog(address_table_, this);

        memory_view_ = new dialogs::MemoryViewerDialog(worker_, target_, this);

        settings_ = new dialogs::SettingsDialog(host_, scanner_->engine(), this);
        settings_->set_dark_theme(dark_theme_active_);
        connect(settings_,
                &dialogs::SettingsDialog::darkThemeChanged,
                this,
                [this](bool dark)
                {
                    dark_theme_active_ = dark;
                    apply_theme(dark ? dark_theme() : light_theme());
                });
        connect(settings_,
                &dialogs::SettingsDialog::alignmentChanged,
                this,
                [this](quint64 alignment)
                {
                    scanner_->set_default_alignment(alignment);
                });

        connect(open_process_action_, &QAction::triggered, this, &MainWindow::show_process_list);
        connect(add_address_action_, &QAction::triggered, this, &MainWindow::show_add_address);
        connect(settings_action_, &QAction::triggered, this, &MainWindow::show_settings);
        connect(about_action_, &QAction::triggered, this, &MainWindow::show_about);
    }

    void MainWindow::show_process_list()
    {
        process_list_->show();
        process_list_->raise();
        process_list_->activateWindow();
    }

    void MainWindow::show_add_address()
    {
        add_address_->show();
        add_address_->raise();
        add_address_->activateWindow();
    }

    void MainWindow::show_settings()
    {
        settings_->show();
        settings_->raise();
        settings_->activateWindow();
    }

    void MainWindow::show_about()
    {
        settings_->select_about();
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
        const bool submitted =
            worker_.submit_freeze(job_id,
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
                                          address_list_->report_freeze_error(process::describe(*frozen.error));
                                      }
                                  });
        if (!submitted)
        {
            freeze_pending_.reset();
        }
    }

} // namespace slopkit::ui
