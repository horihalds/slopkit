#include "ui/main_window.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <QAction>
#include <QCoreApplication>
#include <QEvent>
#include <QFileInfo>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QSplitter>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

#include "app/sandbox.hpp"
#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "debug/controller.hpp"
#include "ui/components/message_box.hpp"
#include "ui/components/widgets.hpp"
#include "ui/dialogs/access_watch.hpp"
#include "ui/dialogs/add_address.hpp"
#include "ui/dialogs/breakpoints.hpp"
#include "ui/dialogs/log.hpp"
#include "ui/dialogs/memory_viewer.hpp"
#include "ui/dialogs/process_list.hpp"
#include "ui/dialogs/settings.hpp"
#include "ui/dialogs/table_settings.hpp"
#include "ui/live_values.hpp"
#include "ui/panels/address_list_panel.hpp"
#include "ui/panels/debug_controls.hpp"
#include "ui/panels/found_list_panel.hpp"
#include "ui/panels/scanner_panel.hpp"
#include "ui/settings.hpp"
#include "ui/text.hpp"
#include "ui/theme.hpp"

namespace slopkit::ui
{

    namespace
    {
        // The production attach question of a practice-target launch: themed,
        // question icon, Yes/No with Yes as the default, since the user has just
        // asked for a target to practise on.
        bool ask_sandbox_attach(QWidget* parent, process::ProcessId pid)
        {
            return widgets::confirm(
                parent,
                QStringLiteral("Attach to the practice target?"),
                QStringLiteral("Attach to the practice target (pid %1)?").arg(static_cast<qulonglong>(pid)),
                widgets::MessageBoxButton::yes);
        }
    } // namespace

    MainWindow::MainWindow(process::AccessWorker&   worker,
                           process::AttachedTarget& target,
                           plugin::PluginHost&      host,
                           SettingsController&      settings,
                           debug::Controller&       debug,
                           const QString&           initial_table_path,
                           QWidget*                 parent)
        : QMainWindow(parent), worker_(worker), target_(target), host_(host), settings_(settings), debug_(debug),
          initial_table_path_(initial_table_path)
    {
        // The production launch and attach question; a test swaps them out.
        sandbox_launcher_ = []
        {
            return app::launch_sandbox();
        };
        sandbox_attach_prompt_ = [](QWidget* parent, process::ProcessId pid)
        {
            return ask_sandbox_attach(parent, pid);
        };

        setWindowTitle(QStringLiteral("slopkit"));
        setWindowIcon(widgets::application_icon());
        resize(748, 768);

        build_actions();
        build_menus();
        build_status_bar();
        build_central();
        build_dialogs();

        // A table path handed over on the command line is opened in preference to
        // the remembered auto-load table.
        if (!initial_table_path_.isEmpty())
        {
            static_cast<void>(open_table_request(initial_table_path_));
        }

        // Event-driven updates only: the tick polls the scan progress, applies a
        // missed drain and submits the freeze pass.
        tick_timer_ = new QTimer(this);
        connect(tick_timer_, &QTimer::timeout, this, &MainWindow::on_tick);
        tick_timer_->start(50);
    }

    MainWindow::~MainWindow() = default;

    bool MainWindow::open_table_request(const QString& path)
    {
        auto parsed = address_list_->parse_table(path);
        if (!parsed)
        {
            // parse_table already reported "Open failed: ..." through the panel.
            return false;
        }

        if (address_table_.empty())
        {
            address_list_->adopt_table(path, std::move(*parsed));
            return true;
        }

        const dialogs::TableConflictInfo info {
            .incoming_path    = path,
            .incoming_entries = parsed->size(),
            .current_path     = address_list_->table_path(),
            .current_entries  = address_table_.size(),
        };

        switch (table_conflict_prompt_(this, info))
        {
        case dialogs::TableConflictChoice::overwrite:
            address_list_->adopt_table(path, std::move(*parsed));
            return true;
        case dialogs::TableConflictChoice::merge:
            static_cast<void>(address_list_->merge_table(*parsed));
            return true;
        case dialogs::TableConflictChoice::cancel:
            address_list_->report_status("Open cancelled.", false);
            return false;
        }
        std::unreachable();
    }

    void MainWindow::set_table_conflict_prompt(TableConflictPrompt prompt)
    {
        table_conflict_prompt_ = std::move(prompt);
    }

    void MainWindow::set_sandbox_launcher(SandboxLauncher launcher)
    {
        sandbox_launcher_ = std::move(launcher);
    }

    void MainWindow::set_sandbox_attach_prompt(SandboxAttachPrompt prompt)
    {
        sandbox_attach_prompt_ = std::move(prompt);
    }

    dialogs::MemoryViewerDialog* MainWindow::memory_viewer() const noexcept
    {
        return memory_view_.get();
    }

    void MainWindow::on_open_table_requested()
    {
        const std::optional<QString> path = address_list_->choose_table_path();
        if (!path.has_value())
        {
            return;
        }
        static_cast<void>(open_table_request(*path));
    }

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
        open_table_action_->setIcon(widgets::action_icon(widgets::ActionIcon::folder));
        open_table_action_->setShortcut(QKeySequence::Open);
        open_table_action_->setShortcutContext(Qt::WindowShortcut);

        save_table_action_ = new QAction(tr("Save Table"), this);
        save_table_action_->setIcon(widgets::action_icon(widgets::ActionIcon::diskette));
        // Shared by the File and Table menus; one action, so no shortcut ambiguity.
        save_table_action_->setShortcut(QKeySequence::Save);
        save_table_action_->setShortcutContext(Qt::WindowShortcut);

        save_table_as_action_ = new QAction(tr("Save Table As"), this);
        save_table_as_action_->setIcon(widgets::action_icon(widgets::ActionIcon::diskette));
        save_table_as_action_->setShortcut(QKeySequence::SaveAs);
        save_table_as_action_->setShortcutContext(Qt::WindowShortcut);

        log_action_ = new QAction(tr("Log"), this);

        settings_action_ = new QAction(tr("Settings"), this);

        breakpoints_action_ = new QAction(tr("Breakpoints"), this);

        access_watch_action_ = new QAction(tr("Access Watch"), this);

        launch_sandbox_action_ = new QAction(tr("Launch Practice Target"), this);

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
        view_menu->addAction(breakpoints_action_);
        view_menu->addAction(access_watch_action_);

        QMenu* help_menu = menuBar()->addMenu(tr("Help"));
        help_menu->addAction(launch_sandbox_action_);
        help_menu->addSeparator();
        help_menu->addAction(about_action_);
    }

    void MainWindow::build_status_bar()
    {
        process_label_ = new QLabel(to_qstring(target_.label()), this);
        process_label_->setObjectName(QStringLiteral("process_label"));
        statusBar()->addWidget(process_label_);

        // The address list reports its outcomes here, in the permanent right
        // slot, so a transient status message never hides it.
        address_status_ = new widgets::StatusLabel(this);
        address_status_->setObjectName(QStringLiteral("address_status"));
        statusBar()->addPermanentWidget(address_status_, 1);
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
        connect(found_list_, &panels::FoundListPanel::accessWatchRequested, this, &MainWindow::start_access_watch);
        connect(scanner_, &panels::ScannerPanel::addAddressRequested, this, &MainWindow::on_add_address_requested);
        connect(scanner_, &panels::ScannerPanel::tableSettingsRequested, this, &MainWindow::show_table_settings);
        connect(scanner_,
                &panels::ScannerPanel::memoryMapApplied,
                this,
                [this](std::vector<process::ModuleInfo> modules)
                {
                    log::debug(log::category::ui, std::format("memory map applied: {} module(s)", modules.size()));
                    found_list_->set_modules(modules);
                    address_list_->set_modules(modules);
                    if (add_address_ != nullptr)
                    {
                        add_address_->set_modules(modules);
                    }
                    debug_.set_modules(modules);
                    memory_view_->set_modules(std::move(modules));
                });
        // Pausing for a scan fights the debugger, so the panel needs the debug
        // session state to gate its pause checkbox.
        connect(&debug_,
                &debug::Controller::stateChanged,
                this,
                [this]
                {
                    scanner_->set_debug_session_active(debug_.state() != debug::Controller::State::idle);
                });
        scanner_->set_debug_session_active(debug_.state() != debug::Controller::State::idle);

        address_list_ = new panels::AddressListPanel(address_table_, worker_, target_, this);
        connect(address_list_, &panels::AddressListPanel::browseRequested, this, &MainWindow::on_memory_view_requested);
        connect(address_list_, &panels::AddressListPanel::accessWatchRequested, this, &MainWindow::start_access_watch);
        connect(address_list_, &panels::AddressListPanel::tableLoaded, this, &MainWindow::on_table_loaded);
        connect(address_list_,
                &panels::AddressListPanel::statusChanged,
                this,
                [this](const QString& message, bool is_error)
                {
                    address_status_->set_status(is_error ? widgets::StatusKind::error : widgets::StatusKind::info,
                                                message);
                });
        connect(open_table_action_, &QAction::triggered, this, &MainWindow::on_open_table_requested);
        connect(launch_sandbox_action_, &QAction::triggered, this, &MainWindow::on_launch_sandbox_requested);
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

        apply_tab_order();
    }

    void MainWindow::apply_tab_order()
    {
        // Declaring the whole junction as one run makes the four cross-panel
        // links unambiguous, whatever order the panels chained their own
        // controls in: the scanner's field run, then the found list's hits table
        // and Memory View button, then the scanner's footer buttons, then the
        // address list.
        widgets::chain_tab_order({scanner_->tab_order_last(),
                                  found_list_->tab_order_first(),
                                  found_list_->tab_order_last(),
                                  scanner_->tab_order_footer_first(),
                                  scanner_->tab_order_footer_last(),
                                  address_list_->tab_order_first()});
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

        add_address_ = new dialogs::AddAddressDialog(address_table_, worker_, this);

        table_settings_ = new dialogs::TableSettingsDialog(address_table_, target_, this);

        memory_view_ = std::make_unique<dialogs::MemoryViewerDialog>(worker_, target_, debug_);

        // The stored window geometry is restored before the window is ever
        // shown; a rejected blob simply leaves the dialog's default size.
        const QByteArray& saved_geometry = settings_.values().memory_view_geometry;
        if (!saved_geometry.isEmpty() && !memory_view_->restoreGeometry(saved_geometry))
        {
            log::warning(log::category::ui, "memory viewer geometry could not be restored; using the default size");
        }
        connect(memory_view_.get(),
                &dialogs::MemoryViewerDialog::geometryChanged,
                this,
                [this](const QByteArray& geometry)
                {
                    settings_.set_memory_view_geometry(geometry);
                });

        // The one live cadence, driven by the window's tick; surfaces register
        // here so a single poll submits a single batched read.
        live_values_ = new LiveValues(worker_, target_, settings_, this);
        live_values_->add_surface(found_list_);
        live_values_->add_surface(address_list_);
        live_values_->add_surface(memory_view_.get());
        connect(memory_view_.get(),
                &dialogs::MemoryViewerDialog::liveRefreshRequested,
                live_values_,
                &LiveValues::request_now);

        connect(memory_view_->debug_controls(),
                &panels::DebugControls::breakpointsRequested,
                this,
                &MainWindow::show_breakpoints);

        log_ = new dialogs::LogDialog(this);

        breakpoints_ = new dialogs::BreakpointsDialog(debug_, this);

        // The Access Watch window is a view over the controller's one watch and
        // over the resolved listing operands; it also routes its own Follow
        // requests into the Memory Viewer.
        access_watch_ = new dialogs::AccessWatchDialog(debug_, worker_, target_, this);
        connect(access_watch_,
                &dialogs::AccessWatchDialog::followRequested,
                this,
                [this](std::uint64_t address)
                {
                    on_memory_view_requested(address);
                });
        // The gate's attach lines (attaching, attached, cancelled, failed) land
        // in the main status area, since no dialog is up while it asks.
        connect(&access_watch_->debug_gate(),
                &ui::DebugSessionGate::progress,
                this,
                [this](const QString& text, bool error)
                {
                    address_list_->report_status(text.toStdString(), error);
                });
        connect(memory_view_.get(),
                &dialogs::MemoryViewerDialog::instructionAccessesResolved,
                this,
                &MainWindow::show_instruction_accesses);
        // The listing command's attach and capture also report into the status line.
        connect(&memory_view_->debug_gate(),
                &ui::DebugSessionGate::progress,
                this,
                [this](const QString& text, bool error)
                {
                    address_list_->report_status(text.toStdString(), error);
                });
        connect(memory_view_.get(),
                &dialogs::MemoryViewerDialog::instructionAccessesProgress,
                this,
                [this](const QString& text, bool error)
                {
                    address_list_->report_status(text.toStdString(), error);
                });

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
                    debug_.set_address_mode(mode);
                    memory_view_->set_address_mode(mode);
                });

        // Apply the mode restored from disk before the window is first shown.
        const ui::AddressMode persisted_mode = settings_.values().address_mode;
        found_list_->set_address_mode(persisted_mode);
        address_list_->set_address_mode(persisted_mode);
        debug_.set_address_mode(persisted_mode);
        memory_view_->set_address_mode(persisted_mode);

        connect(open_process_action_, &QAction::triggered, this, &MainWindow::show_process_list);
        connect(log_action_, &QAction::triggered, this, &MainWindow::show_log);
        connect(breakpoints_action_, &QAction::triggered, this, &MainWindow::show_breakpoints);
        connect(access_watch_action_, &QAction::triggered, this, &MainWindow::show_access_watch);
        connect(settings_action_, &QAction::triggered, this, &MainWindow::show_settings);
        connect(about_action_, &QAction::triggered, this, &MainWindow::show_about);

        // Seed the remembered table and the dialog directory, then wire the
        // remembering slots so the seed value is not written back.
        const Settings& persisted = settings_.values();
        address_list_->set_table_path(persisted.last_table_path);
        address_list_->set_dialog_directory(persisted.last_directory);
        log_->set_dialog_directory(persisted.last_directory);

        connect(address_list_, &panels::AddressListPanel::tablePathChanged, this, &MainWindow::remember_table_path);
        connect(log_, &dialogs::LogDialog::logSaved, this, &MainWindow::remember_file_path);

        // With the switch on, the remembered table is loaded exactly like a manual
        // Ctrl+O; a failure only reports and keeps the path remembered. An explicit
        // launch path wins over the remembered one.
        if (initial_table_path_.isEmpty() && persisted.auto_load_last_table && !persisted.last_table_path.isEmpty())
        {
            static_cast<void>(address_list_->load_table(persisted.last_table_path));
        }
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

    void MainWindow::show_table_settings()
    {
        log::debug(log::category::ui, "opening Table Settings dialog");
        table_settings_->show();
        table_settings_->raise();
        table_settings_->activateWindow();
    }

    void MainWindow::show_log()
    {
        log::debug(log::category::ui, "opening Log dialog");
        log_->show();
        log_->raise();
        log_->activateWindow();
    }

    void MainWindow::show_breakpoints()
    {
        if (breakpoints_ == nullptr)
        {
            return;
        }
        breakpoints_->refresh();
        breakpoints_->show();
        breakpoints_->raise();
        breakpoints_->activateWindow();
    }

    void MainWindow::show_access_watch()
    {
        if (access_watch_ == nullptr)
        {
            return;
        }
        access_watch_->show();
        access_watch_->raise();
        access_watch_->activateWindow();
    }

    void MainWindow::start_access_watch(std::uint64_t address, std::size_t width, debug::Kind kind)
    {
        if (access_watch_ == nullptr)
        {
            return;
        }
        // The window is shown by the dialog itself once the watch is armed, so a
        // declined or failed attach never pops an empty window.
        access_watch_->arm_watch(address, kind, ui::watch_size(width));
    }

    void MainWindow::show_instruction_accesses(std::uint64_t                   instruction,
                                               std::size_t                     instruction_length,
                                               std::vector<ui::ResolvedAccess> accesses)
    {
        if (access_watch_ == nullptr)
        {
            return;
        }
        show_access_watch();
        access_watch_->show_instruction_accesses(instruction, instruction_length, std::move(accesses));
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

    void MainWindow::on_launch_sandbox_requested()
    {
        const auto result = sandbox_launcher_();
        if (!result)
        {
            log::warning(log::category::ui, std::format("launch practice target failed: {}", result.error()));
            address_list_->report_status(result.error(), true);
            return;
        }
        log::info(log::category::ui, std::format("practice target started (pid {})", *result));
        address_list_->report_status(std::format("Practice target started (pid {}).", *result), false);

        // Ask before taking the still-running target over; a declined question
        // keeps the launch and leaves any current target untouched.
        const auto pid = static_cast<process::ProcessId>(*result);
        if (!sandbox_attach_prompt_(this, pid))
        {
            return;
        }
        lookup_and_attach(
            [pid](std::span<const process::ProcessInfo> processes)
            {
                const auto found = std::ranges::find(processes, pid, &process::ProcessInfo::pid);
                return found == processes.end() ? nullptr : &*found;
            },
            "Practice target",
            std::string {"Practice target attach: the launched process was not found."});
    }

    void MainWindow::remember_table_path(const QString& path)
    {
        settings_.set_last_table_path(path);
        settings_.set_last_directory(QFileInfo(path).absolutePath());
    }

    void MainWindow::remember_file_path(const QString& path)
    {
        settings_.set_last_directory(QFileInfo(path).absolutePath());
    }

    bool MainWindow::event(QEvent* event)
    {
        if (event->type() == QEvent::WindowActivate)
        {
            scanner_->focus_value_input();
        }
        return QMainWindow::event(event);
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
        live_values_->poll();
    }

    void MainWindow::on_memory_view_requested(quint64 address)
    {
        log::debug(log::category::ui, std::format("opening Memory Viewer at {:X}", address));
        memory_view_->set_address(address);
        memory_view_->show();
        memory_view_->raise();
        memory_view_->activateWindow();
    }

    void MainWindow::on_add_address_requested()
    {
        show_add_address();
    }

    void MainWindow::on_table_loaded()
    {
        const table::TableSettings& settings = address_table_.settings();

        // Which field identifies the process is the toggle's choice; a ticked
        // toggle without a path keeps the pre-exe_path behaviour.
        const bool use_path = settings.match_exe_path && !settings.exe_path.empty();

        if (!settings.auto_attach)
        {
            return;
        }
        if (!use_path && settings.target_process.empty())
        {
            return;
        }

        if (target_.valid())
        {
            log::info(log::category::ui, std::format("auto attach skipped: already attached to {}", target_.label()));
            address_list_->report_status("Table auto attach skipped: already attached.", false);
            return;
        }

        const std::string identifier     = use_path ? settings.exe_path : settings.target_process;
        const bool        match_exe_path = settings.match_exe_path;

        // The listing picks the process; the shared step attaches to it.
        lookup_and_attach(
            [use_path, match_exe_path, identifier](std::span<const process::ProcessInfo> processes)
            {
                // A ticked toggle without a path falls back to the old
                // name-or-executable-basename match.
                return use_path ? process::match_process_by_exe_path(processes, identifier)
                                : process::match_process_by_name(processes, identifier, match_exe_path);
            },
            "Table auto",
            std::format("Table auto attach: no process matching {} '{}'.",
                        use_path ? "executable path" : "process name",
                        identifier));
    }

    void MainWindow::lookup_and_attach(ProcessMatcher match, std::string_view context, std::string missing)
    {
        // One listing at a time; a newer request supersedes whatever is in flight.
        if (target_lookup_pending_.has_value())
        {
            return;
        }

        const std::string    context_text = std::string(context);
        const process::JobId job_id       = worker_.next_job_id();
        target_lookup_pending_            = job_id;

        log::debug(log::category::ui, std::format("{} attach: listing processes", context_text));

        const bool submitted = worker_.submit_list(
            job_id,
            [this, job_id, context_text, missing, match = std::move(match)](process::JobResult&& result)
            {
                if (target_lookup_pending_ != job_id)
                {
                    return; // Superseded or shut down.
                }
                target_lookup_pending_.reset();

                auto& listed = std::get<process::ListResult>(result);
                if (listed.error)
                {
                    log::warning(log::category::ui,
                                 std::format("{} attach failed: {}", context_text, process::describe(*listed.error)));
                    address_list_->report_status(
                        std::format("{} attach failed: {}", context_text, process::describe(*listed.error)), true);
                    return;
                }

                const process::ProcessInfo* found = match(listed.processes);
                if (found == nullptr)
                {
                    log::warning(log::category::ui, missing);
                    address_list_->report_status(missing, true);
                    return;
                }
                submit_process_attach(*found, context_text);
            });
        if (!submitted)
        {
            target_lookup_pending_.reset();
            log::warning(log::category::ui, std::format("{} attach unavailable", context_text));
            address_list_->report_status(std::format("{} attach unavailable.", context_text), true);
        }
    }

    void MainWindow::submit_process_attach(const process::ProcessInfo& info, std::string_view context)
    {
        if (auto_attach_pending_.has_value())
        {
            return; // Another attach already runs.
        }

        // Keep the process identity before the listing result goes away.
        const process::ProcessId pid      = info.pid;
        const std::string        pname    = info.name;
        const std::string        exe_path = info.exe_path;
        const std::string        plugin   = info.plugin_id;
        const std::string        prefix(context);

        const process::JobId attach_job = worker_.next_job_id();
        auto_attach_pending_            = attach_job;

        log::debug(log::category::ui, std::format("{} attach: pid {} via {}", prefix, pid, plugin));

        const bool submitted = worker_.submit_attach_app(
            attach_job,
            pid,
            plugin,
            [this, attach_job, pid, pname, exe_path, prefix](process::JobResult&& attach_result)
            {
                if (auto_attach_pending_ != attach_job)
                {
                    return; // Superseded or shut down.
                }
                auto_attach_pending_.reset();

                const auto& attached = std::get<process::AttachResult>(attach_result);
                if (attached.error)
                {
                    target_.clear();
                    log::warning(
                        log::category::ui,
                        std::format("{} attach to pid {} failed: {}", prefix, pid, process::describe(*attached.error)));
                    address_list_->report_status(
                        std::format("{} attach failed: {}", prefix, process::describe(*attached.error)), true);
                    refresh_target_label();
                    return;
                }

                target_.clear();
                target_.pid          = pid;
                target_.name         = pname;
                target_.exe_path     = exe_path;
                target_.plugin_id    = attached.info->plugin_id;
                target_.method       = attached.info->method;
                target_.session_live = true;
                log::info(log::category::ui,
                          std::format("{} attached to pid {} via {}", prefix, pid, attached.info->plugin_id));
                refresh_target_label();
                if (attached.info->read_error)
                {
                    log::warning(log::category::ui,
                                 std::format("{} attach to pid {} cannot read memory: {}",
                                             prefix,
                                             pid,
                                             process::describe(*attached.info->read_error)));
                    address_list_->report_status(std::format("Attached to {}; memory not readable ({}).",
                                                             pname,
                                                             process::describe(*attached.info->read_error)),
                                                 true);
                }
                else
                {
                    address_list_->report_status(std::format("Attached to {}.", pname), false);
                }
            });
        if (!submitted)
        {
            auto_attach_pending_.reset();
            log::warning(log::category::ui, std::format("{} attach unavailable", prefix));
            address_list_->report_status(std::format("{} attach unavailable.", prefix), true);
        }
    }

    void MainWindow::refresh_target_label()
    {
        process_label_->setText(to_qstring(target_.label()));
        found_list_->set_target_attached(target_.valid());
        table_settings_->refresh_target();
        // The debug gating depends on the attached target, so a fresh attach or
        // detach must update the viewer's Start/Stop surfaces, not only the
        // controller's own state signals.
        memory_view_->refresh_target_state();
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
