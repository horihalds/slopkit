#include <catch2/catch.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPalette>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QString>
#include <QTableView>
#include <QToolBar>

#include "plugin/plugin_host.hpp"
#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/plugin_access.hpp"
#include "ui/components/widgets.hpp"
#include "ui/dialogs/process_list.hpp"
#include "ui/main_window.hpp"
#include "ui/models/address_table_model.hpp"
#include "ui/models/found_results_model.hpp"
#include "ui/panels/found_list_panel.hpp"
#include "ui/panels/scanner_panel.hpp"
#include "ui/theme.hpp"

namespace
{
    // A QApplication may only exist once per process; Catch2 normally runs each
    // case in its own process, but the binary accepts several.
    QApplication& application()
    {
        static int          argc      = 1;
        static char         program[] = "slopkit_tests";
        static char*        argv[]    = {program, nullptr};
        static QApplication instance(argc, argv);
        return instance;
    }

    void check_all_roles_defined(const slopkit::ui::Theme& theme)
    {
        const QColor roles[] = {theme.background,
                                theme.surface,
                                theme.surface_hover,
                                theme.text,
                                theme.text_muted,
                                theme.accent,
                                theme.accent_hover,
                                theme.accent_active,
                                theme.on_accent,
                                theme.border,
                                theme.success,
                                theme.warning,
                                theme.error};
        for (const QColor& role : roles)
        {
            CHECK(role.isValid());
        }
    }

    QList<QString> action_texts(const QList<QAction*>& actions)
    {
        QList<QString> texts;
        for (QAction* action : actions)
        {
            if (!action->isSeparator())
            {
                texts.append(action->text());
            }
        }
        return texts;
    }

    // A tiny in-memory target the Process List dialog can attach to in tests.
    class UiFakeBackend final : public slopkit::process::SessionBackend
    {
    public:
        [[nodiscard]] slopkit::process::ProcessId pid() const noexcept override
        {
            return 42;
        }

        [[nodiscard]] std::string_view plugin_id() const noexcept override
        {
            return "fake";
        }

        [[nodiscard]] slopkit::process::AccessMethod advertised_methods() const noexcept override
        {
            return slopkit::process::AccessMethod::procfs_mem;
        }

        [[nodiscard]] slopkit::process::AccessMethod last_method() const noexcept override
        {
            return slopkit::process::AccessMethod::procfs_mem;
        }

        std::expected<std::vector<std::byte>, slopkit::process::AccessError> read(std::uint64_t, std::size_t) override
        {
            return std::vector<std::byte> {};
        }

        std::expected<std::size_t, slopkit::process::AccessError> write(std::uint64_t,
                                                                        std::span<const std::byte>) override
        {
            return std::size_t {};
        }

        std::expected<std::vector<slopkit::process::ModuleInfo>, slopkit::process::AccessError> modules() override
        {
            return std::vector<slopkit::process::ModuleInfo> {};
        }

        std::expected<std::vector<slopkit::process::ThreadInfo>, slopkit::process::AccessError> threads() override
        {
            return std::vector<slopkit::process::ThreadInfo> {};
        }

        std::expected<std::vector<slopkit::process::RegionInfo>, slopkit::process::AccessError> regions() override
        {
            return std::vector<slopkit::process::RegionInfo> {};
        }
    };

    // A ProcessAccess serving a fixed process list that can be told to fail the
    // attach, so the dialog can be driven without a real target.
    class UiFakeAccess final : public slopkit::process::ProcessAccess
    {
    public:
        std::vector<slopkit::process::ProcessInfo> processes;
        bool                                       attach_fails {false};
        std::atomic<int>                           attach_calls {0};
        std::atomic<int>                           list_calls {0};

        std::expected<std::vector<slopkit::process::ProcessInfo>, slopkit::process::AccessError>
        list_processes() override
        {
            ++list_calls;
            return processes;
        }

        std::expected<slopkit::process::Session, slopkit::process::AccessError> attach(slopkit::process::ProcessId,
                                                                                       std::string_view) override
        {
            ++attach_calls;
            if (attach_fails)
            {
                return std::unexpected(slopkit::process::AccessError::permission_denied);
            }
            return slopkit::process::Session {std::make_unique<UiFakeBackend>()};
        }
    };

    std::vector<slopkit::process::ProcessInfo> sample_processes()
    {
        slopkit::process::ProcessInfo first;
        first.pid       = 10;
        first.name      = "alpha";
        first.exe_path  = "/usr/bin/alpha";
        first.plugin_id = "fake";
        first.claimants = {"fake"};

        slopkit::process::ProcessInfo second;
        second.pid       = 20;
        second.name      = "beta";
        second.exe_path  = "/usr/bin/beta";
        second.plugin_id = "fake";
        second.claimants = {"fake"};

        return {first, second};
    }

    // Drains the worker (and the Qt event loop) until `done` holds or the
    // timeout elapses. The dialog completes its jobs through AccessWorker.
    template<typename Predicate>
    bool pump_ui(slopkit::process::AccessWorker& worker, Predicate done)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline)
        {
            worker.drain();
            QCoreApplication::processEvents();
            if (done())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return done();
    }
} // namespace

TEST_CASE("both themes define every colour role", "[ui]")
{
    const auto dark  = slopkit::ui::dark_theme();
    const auto light = slopkit::ui::light_theme();

    check_all_roles_defined(dark);
    check_all_roles_defined(light);

    CHECK(dark.background != light.background);
    CHECK(dark.surface != light.surface);
    CHECK(dark.text != light.text);
    CHECK(dark.accent != light.accent);
}

TEST_CASE("make_palette maps the colour roles onto the Qt palette", "[ui]")
{
    const auto     theme   = slopkit::ui::light_theme();
    const QPalette palette = slopkit::ui::make_palette(theme);

    CHECK(palette.color(QPalette::Window) == theme.background);
    CHECK(palette.color(QPalette::Base) == theme.surface);
    CHECK(palette.color(QPalette::Button) == theme.surface);
    CHECK(palette.color(QPalette::WindowText) == theme.text);
    CHECK(palette.color(QPalette::Text) == theme.text);
    CHECK(palette.color(QPalette::ButtonText) == theme.text);
    CHECK(palette.color(QPalette::Mid) == theme.border);
    CHECK(palette.color(QPalette::Highlight) == theme.accent);
    CHECK(palette.color(QPalette::HighlightedText) == theme.on_accent);
    CHECK(palette.color(QPalette::PlaceholderText) == theme.text_muted);
    CHECK(palette.color(QPalette::Disabled, QPalette::Text) == theme.text_muted);

    const QPalette dark = slopkit::ui::make_palette(slopkit::ui::dark_theme());
    CHECK(dark.color(QPalette::Window) != palette.color(QPalette::Window));
}

TEST_CASE("apply_theme installs the palette and caches the theme", "[ui]")
{
    application();

    slopkit::ui::apply_theme(slopkit::ui::light_theme());
    CHECK(slopkit::ui::active_theme().background == slopkit::ui::light_theme().background);
    CHECK(QGuiApplication::palette().color(QPalette::Window) == slopkit::ui::light_theme().background);

    slopkit::ui::apply_theme(slopkit::ui::dark_theme());
    CHECK(slopkit::ui::active_theme().background == slopkit::ui::dark_theme().background);
    CHECK(QGuiApplication::palette().color(QPalette::Window) == slopkit::ui::dark_theme().background);
}

TEST_CASE("themed widgets follow a theme switch", "[ui]")
{
    application();

    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    slopkit::ui::widgets::StatusLabel status;
    status.set_status(slopkit::ui::widgets::StatusKind::error, QStringLiteral("boom"));
    CHECK(status.palette().color(QPalette::WindowText) == slopkit::ui::dark_theme().error);

    slopkit::ui::widgets::PrimaryButton button(QStringLiteral("Scan"));
    CHECK(button.palette().color(QPalette::Button) == slopkit::ui::dark_theme().accent);

    // A theme switch changes the application palette, which Qt propagates to
    // every widget as a palette-change event; deliver it here so the check does
    // not depend on the event loop's timing.
    slopkit::ui::apply_theme(slopkit::ui::light_theme());
    QEvent palette_change(QEvent::PaletteChange);
    QCoreApplication::sendEvent(&status, &palette_change);
    QCoreApplication::sendEvent(&button, &palette_change);

    CHECK(status.palette().color(QPalette::WindowText) == slopkit::ui::light_theme().error);
    CHECK(button.palette().color(QPalette::Button) == slopkit::ui::light_theme().accent);
}

TEST_CASE("the found-results model mirrors a snapshot", "[ui]")
{
    application();

    slopkit::ui::models::FoundResultsModel model;

    slopkit::scan::ScanConfig config;
    config.value_type = slopkit::scan::ValueType::int32;

    slopkit::scan::ScanSnapshot snapshot;
    snapshot.hit_count = 3; // Two of the three matches are on the display page.
    snapshot.hits.push_back(slopkit::scan::ScanHit {
        0x2000, std::vector<std::byte> {std::byte {2}, std::byte {0}, std::byte {0}, std::byte {0}},
          {}
    });
    snapshot.hits.push_back(slopkit::scan::ScanHit {
        0x1000, std::vector<std::byte> {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}},
          {}
    });

    model.set_snapshot(snapshot, config);

    CHECK(model.rowCount() == 2);
    CHECK(model.columnCount() == 3);
    CHECK(model.headerData(slopkit::ui::models::FoundResultsModel::address, Qt::Horizontal, Qt::DisplayRole).toString()
          == QStringLiteral("Address"));

    // Default order is by address, ascending.
    CHECK(model.hit_at(0)->address == 0x1000);
    CHECK(model.data(model.index(0, slopkit::ui::models::FoundResultsModel::address), Qt::DisplayRole).toString()
          == QStringLiteral("0x1000"));
    CHECK(model.data(model.index(1, slopkit::ui::models::FoundResultsModel::value), Qt::DisplayRole).toString()
          == QStringLiteral("2"));
    CHECK(model.data(model.index(0, slopkit::ui::models::FoundResultsModel::previous), Qt::DisplayRole)
              .toString()
              .isEmpty());

    model.sort(slopkit::ui::models::FoundResultsModel::address, Qt::DescendingOrder);
    CHECK(model.hit_at(0)->address == 0x2000);

    model.clear();
    CHECK(model.rowCount() == 0);
    CHECK(model.hit_at(0) == nullptr);
}

TEST_CASE("the address-table model edits the table", "[ui]")
{
    application();

    slopkit::table::AddressTable table;
    slopkit::table::AddressEntry entry;
    entry.address = 0x1040;
    entry.type    = slopkit::scan::ValueType::int32;
    entry.bytes   = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
    table.add(entry);

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::models::AddressTableModel model {table, worker, target};

    CHECK(model.rowCount() == 1);
    CHECK(model.columnCount() == 5);
    CHECK(model.data(model.index(0, slopkit::ui::models::AddressTableModel::address), Qt::DisplayRole).toString()
          == QStringLiteral("0x1040"));
    CHECK(model.data(model.index(0, slopkit::ui::models::AddressTableModel::type), Qt::DisplayRole).toString()
          == QStringLiteral("4 Bytes"));
    CHECK(model.data(model.index(0, slopkit::ui::models::AddressTableModel::value), Qt::DisplayRole).toString()
          == QStringLiteral("1"));
    CHECK(model.data(model.index(0, slopkit::ui::models::AddressTableModel::frozen), Qt::CheckStateRole).toInt()
          == Qt::Unchecked);

    // The Frozen column doubles as the freeze toggle.
    REQUIRE(
        model.setData(model.index(0, slopkit::ui::models::AddressTableModel::frozen), Qt::Checked, Qt::CheckStateRole));
    CHECK(table.entries()[0].active);
    CHECK(model.data(model.index(0, slopkit::ui::models::AddressTableModel::frozen), Qt::CheckStateRole).toInt()
          == Qt::Checked);

    // A description edit lands in the table.
    REQUIRE(model.setData(
        model.index(0, slopkit::ui::models::AddressTableModel::description), QStringLiteral("health"), Qt::EditRole));
    CHECK(table.entries()[0].description == "health");

    // Without a target the value edit is refused and the model says why.
    QString message;
    bool    is_error = false;
    QObject::connect(&model,
                     &slopkit::ui::models::AddressTableModel::statusChanged,
                     &model,
                     [&](const QString& text, bool error)
                     {
                         message  = text;
                         is_error = error;
                     });
    CHECK_FALSE(model.setData(
        model.index(0, slopkit::ui::models::AddressTableModel::value), QStringLiteral("42"), Qt::EditRole));
    CHECK(is_error);
    CHECK(message.contains(QStringLiteral("Not attached")));
}

TEST_CASE("the main window shell is built", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::ui::MainWindow          window {worker, target, host};

    // The five menus mirror the previous top bar.
    const QList<QAction*> menus = window.menuBar()->actions();
    REQUIRE(menus.size() == 5);
    CHECK(menus[0]->text() == QStringLiteral("File"));
    CHECK(menus[1]->text() == QStringLiteral("Edit"));
    CHECK(menus[2]->text() == QStringLiteral("Table"));
    CHECK(menus[3]->text() == QStringLiteral("D3D"));
    CHECK(menus[4]->text() == QStringLiteral("Help"));

    CHECK(action_texts(menus[0]->menu()->actions())
          == QList<QString> {QStringLiteral("Open Process..."),
                             QStringLiteral("Open Table..."),
                             QStringLiteral("Save Table..."),
                             QStringLiteral("Quit")});

    // The toolbar is gone; the file commands live only in the menus now.
    CHECK(window.findChild<QToolBar*>(QStringLiteral("main_toolbar")) == nullptr);

    // The Edit menu is now the only route to the settings dialog.
    CHECK(menus[1]->menu()->actions().last()->text() == QStringLiteral("Settings..."));

    // Ctrl+T / Ctrl+O / Ctrl+S are bound to the file commands.
    const QList<QAction*> file_actions = menus[0]->menu()->actions();
    REQUIRE(file_actions.size() == 5);
    CHECK(file_actions[0]->shortcut() == QKeySequence(QStringLiteral("Ctrl+T")));
    CHECK(file_actions[1]->shortcut() == QKeySequence(QKeySequence::Open));
    CHECK(file_actions[2]->shortcut() == QKeySequence(QKeySequence::Save));

    // The Table menu reuses the very same save action, so its shortcut is not
    // registered twice.
    CHECK(menus[2]->menu()->actions().last() == file_actions[2]);

    // The status bar shows the detached target and the scan progress.
    auto* process_label = window.statusBar()->findChild<QLabel*>();
    REQUIRE(process_label != nullptr);
    CHECK(process_label->text() == QStringLiteral("No Process Selected"));

    auto* progress = window.statusBar()->findChild<QProgressBar*>();
    REQUIRE(progress != nullptr);
    CHECK(progress->value() == 0);

    // Two split zones inside the middle zone, the address list below them.
    auto* vertical = qobject_cast<QSplitter*>(window.centralWidget());
    REQUIRE(vertical != nullptr);
    CHECK(vertical->orientation() == Qt::Vertical);
    REQUIRE(vertical->count() == 2);

    auto* middle = qobject_cast<QSplitter*>(vertical->widget(0));
    REQUIRE(middle != nullptr);
    CHECK(middle->orientation() == Qt::Horizontal);
    CHECK(middle->count() == 2);

    // The middle zone holds the live found list and the scanner controls.
    auto* found_list = window.findChild<slopkit::ui::panels::FoundListPanel*>();
    REQUIRE(found_list != nullptr);
    CHECK(window.findChild<slopkit::ui::panels::ScannerPanel*>() != nullptr);

    auto* found_header = found_list->findChild<QLabel*>();
    REQUIRE(found_header != nullptr);
    CHECK(found_header->text() == QStringLiteral("Found: 0"));

    // A live theme switch re-installs the application palette.
    slopkit::ui::apply_theme(slopkit::ui::light_theme());
    QCoreApplication::processEvents();
    CHECK(QGuiApplication::palette().color(QPalette::Window) == slopkit::ui::light_theme().background);
}

TEST_CASE("the process list dialog focuses the filter box and preselects the top result", "[ui]")
{
    application();

    UiFakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::dialogs::ProcessListDialog dialog {worker, target};
    dialog.show();

    auto* search = dialog.findChild<QLineEdit*>();
    auto* table  = dialog.findChild<QTableView*>();
    REQUIRE(search != nullptr);
    REQUIRE(table != nullptr);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table->model() != nullptr && table->model()->rowCount() == 2
                            && table->currentIndex().row() == 0;
                    }));

    // The filter box owns the keyboard focus and the top result is selected.
    CHECK(dialog.focusWidget() == search);
    CHECK(table->currentIndex().row() == 0);

    // Enter in the filter box attaches the preselected (top) process.
    QKeyEvent enter {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QCoreApplication::sendEvent(search, &enter);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return target.valid();
                    }));
    CHECK(target.pid == 10);

    // A successful attach dismisses the picker.
    CHECK_FALSE(dialog.isVisible());

    dialog.close();
}

TEST_CASE("double-clicking a process row attaches it", "[ui]")
{
    application();

    UiFakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::dialogs::ProcessListDialog dialog {worker, target};
    dialog.show();

    auto* table = dialog.findChild<QTableView*>();
    REQUIRE(table != nullptr);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table->model() != nullptr && table->model()->rowCount() == 2;
                    }));

    // Double-click the second row: a press parks the index, the double-click
    // activates it.
    const QModelIndex second = table->model()->index(1, slopkit::ui::dialogs::ProcessListModel::pid);
    const QRect       rect   = table->visualRect(second);
    REQUIRE_FALSE(rect.isEmpty());
    const QPoint pos = rect.center();

    QMouseEvent press {QEvent::MouseButtonPress,
                       pos,
                       table->viewport()->mapToGlobal(pos),
                       Qt::LeftButton,
                       Qt::LeftButton,
                       Qt::NoModifier};
    QCoreApplication::sendEvent(table->viewport(), &press);
    QMouseEvent dbl {QEvent::MouseButtonDblClick,
                     pos,
                     table->viewport()->mapToGlobal(pos),
                     Qt::LeftButton,
                     Qt::LeftButton,
                     Qt::NoModifier};
    QCoreApplication::sendEvent(table->viewport(), &dbl);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return target.valid();
                    }));
    CHECK(target.pid == 20);

    dialog.close();
}

TEST_CASE("the process list dialog ignores Enter when the list is empty", "[ui]")
{
    application();

    UiFakeAccess                     access; // Empty listing.
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::dialogs::ProcessListDialog dialog {worker, target};
    dialog.show();

    auto* search = dialog.findChild<QLineEdit*>();
    REQUIRE(search != nullptr);

    QKeyEvent enter {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};

    // No selection yet: Enter must not submit an attach.
    QCoreApplication::sendEvent(search, &enter);
    QCoreApplication::processEvents();
    CHECK(access.attach_calls.load() == 0);
    CHECK_FALSE(target.valid());

    // ...nor once the empty listing has landed.
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return access.list_calls.load() >= 1;
                    }));
    QCoreApplication::processEvents();
    QCoreApplication::sendEvent(search, &enter);
    QCoreApplication::processEvents();
    CHECK(access.attach_calls.load() == 0);
    CHECK_FALSE(target.valid());

    dialog.close();
}

TEST_CASE("a failed attach leaves the process list dialog open", "[ui]")
{
    application();

    UiFakeAccess access;
    access.processes    = sample_processes();
    access.attach_fails = true;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::dialogs::ProcessListDialog dialog {worker, target};
    dialog.show();

    auto* search = dialog.findChild<QLineEdit*>();
    auto* table  = dialog.findChild<QTableView*>();
    REQUIRE(search != nullptr);
    REQUIRE(table != nullptr);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table->model() != nullptr && table->model()->rowCount() == 2;
                    }));

    QKeyEvent enter {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QCoreApplication::sendEvent(search, &enter);

    // The failure is surfaced in the status area and the picker stays up.
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        for (auto* label : dialog.findChildren<slopkit::ui::widgets::StatusLabel*>())
                        {
                            if (label->text().contains(QStringLiteral("attach failed")))
                            {
                                return true;
                            }
                        }
                        return false;
                    }));

    CHECK_FALSE(target.valid());
    CHECK(dialog.isVisible());

    dialog.close();
}

TEST_CASE("detaching does not close the process list dialog", "[ui]")
{
    application();

    UiFakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    target.pid          = 10;
    target.name         = "alpha";
    target.plugin_id    = "fake";
    target.method       = slopkit::process::AccessMethod::procfs_mem;
    target.session_live = true;

    slopkit::ui::dialogs::ProcessListDialog dialog {worker, target};
    dialog.show();

    auto* table = dialog.findChild<QTableView*>();
    REQUIRE(table != nullptr);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table->model() != nullptr && table->model()->rowCount() == 2;
                    }));

    // Row 0 (pid 10) is the attached process, so the button detaches it.
    QPushButton* attach_button = nullptr;
    for (auto* button : dialog.findChildren<QPushButton*>())
    {
        if (button->text() == QStringLiteral("Detach"))
        {
            attach_button = button;
        }
    }
    REQUIRE(attach_button != nullptr);
    attach_button->click();

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return !target.valid();
                    }));
    CHECK(dialog.isVisible());

    dialog.close();
}

TEST_CASE("the process model keeps desktop applications in the processes view", "[ui]")
{
    application();

    slopkit::ui::dialogs::ProcessListModel model;

    slopkit::process::ProcessInfo mumble;
    mumble.pid      = 100;
    mumble.name     = "Mumble";
    mumble.exe_path = "/usr/bin/mumble";

    slopkit::process::ProcessInfo shell;
    shell.pid      = 200;
    shell.name     = "bash";
    shell.exe_path = "/usr/bin/bash";

    model.set_processes({mumble, shell});
    model.set_application_index({"mumble"});

    // The Processes view lists everything, desktop application included.
    CHECK(model.rowCount() == 2);
    CHECK(model.row_for_pid(100) >= 0);
    CHECK(model.row_for_pid(200) >= 0);

    // ...so a name filter finds the application there too.
    model.set_search(QStringLiteral("mumble"));
    CHECK(model.rowCount() == 1);
    REQUIRE(model.process_at(0) != nullptr);
    CHECK(model.process_at(0)->pid == 100);

    model.set_search(QString());

    // The Applications view restricts to desktop entries.
    model.set_applications_only(true);
    CHECK(model.rowCount() == 1);
    REQUIRE(model.process_at(0) != nullptr);
    CHECK(model.process_at(0)->pid == 100);
}

TEST_CASE("typing in the filter selects the first result", "[ui]")
{
    application();

    UiFakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::dialogs::ProcessListDialog dialog {worker, target};
    dialog.show();

    auto* search = dialog.findChild<QLineEdit*>();
    auto* table  = dialog.findChild<QTableView*>();
    REQUIRE(search != nullptr);
    REQUIRE(table != nullptr);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table->model() != nullptr && table->model()->rowCount() == 2;
                    }));

    // Park the selection on the second row...
    table->setCurrentIndex(table->model()->index(1, slopkit::ui::dialogs::ProcessListModel::pid));
    REQUIRE(table->currentIndex().row() == 1);

    // ...then type a filter that still matches both rows.
    search->setText(QStringLiteral("usr/bin"));

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table->model() != nullptr && table->model()->rowCount() == 2
                            && table->currentIndex().row() == 0;
                    }));
    CHECK(table->currentIndex().row() == 0);

    dialog.close();
}
