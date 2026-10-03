#include <catch2/catch.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QComboBox>
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
#include <QScrollBar>
#include <QSplitter>
#include <QStatusBar>
#include <QString>
#include <QTableView>
#include <QToolBar>
#include <QToolButton>

#include "plugin/plugin_host.hpp"
#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/plugin_access.hpp"
#include "scan/source.hpp"
#include "ui/components/widgets.hpp"
#include "ui/dialogs/add_address.hpp"
#include "ui/dialogs/memory_viewer.hpp"
#include "ui/dialogs/process_list.hpp"
#include "ui/main_window.hpp"
#include "ui/models/address_table_model.hpp"
#include "ui/models/found_results_model.hpp"
#include "ui/panels/address_list_panel.hpp"
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
        std::vector<slopkit::process::ModuleInfo> module_list;
        std::vector<slopkit::process::RegionInfo> region_list;

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
            return module_list;
        }

        std::expected<std::vector<slopkit::process::ThreadInfo>, slopkit::process::AccessError> threads() override
        {
            return std::vector<slopkit::process::ThreadInfo> {};
        }

        std::expected<std::vector<slopkit::process::RegionInfo>, slopkit::process::AccessError> regions() override
        {
            return region_list;
        }
    };

    // A ProcessAccess serving a fixed process list that can be told to fail the
    // attach, so the dialog can be driven without a real target.
    class UiFakeAccess final : public slopkit::process::ProcessAccess
    {
    public:
        std::vector<slopkit::process::ProcessInfo> processes;
        std::vector<slopkit::process::ModuleInfo>  modules;
        std::vector<slopkit::process::RegionInfo>  regions;
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
            auto backend         = std::make_unique<UiFakeBackend>();
            backend->module_list = modules;
            backend->region_list = regions;
            return slopkit::process::Session {std::move(backend)};
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

    // The scanner panel's Start/Stop fields, located by their placeholder text.
    QLineEdit* address_field(QWidget& panel, const char* placeholder)
    {
        for (auto* edit : panel.findChildren<QLineEdit*>())
        {
            if (edit->placeholderText() == QString::fromUtf8(placeholder))
            {
                return edit;
            }
        }
        return nullptr;
    }

    // Attaches the app-wide session to the fake backend on pid 42.
    void attach_app_session(slopkit::process::AccessWorker& worker)
    {
        bool attached = false;
        worker.submit_attach_app(worker.next_job_id(),
                                 42,
                                 "fake",
                                 [&](slopkit::process::JobResult&&)
                                 {
                                     attached = true;
                                 });
        REQUIRE(pump_ui(worker,
                        [&]
                        {
                            return attached;
                        }));
    }

    slopkit::process::AttachedTarget fake_target()
    {
        slopkit::process::AttachedTarget target;
        target.pid          = 42;
        target.name         = "fake";
        target.plugin_id    = "fake";
        target.method       = slopkit::process::AccessMethod::procfs_mem;
        target.session_live = true;
        return target;
    }

    // The scan-range dropdown, located by its tooltip.
    QComboBox* range_combo(QWidget& panel)
    {
        for (auto* combo : panel.findChildren<QComboBox*>())
        {
            if (combo->toolTip() == QStringLiteral("Whole process or a single loaded module"))
            {
                return combo;
            }
        }
        return nullptr;
    }

    // The nearest widgets::Panel ancestor of a widget, or nullptr.
    QWidget* ancestor_panel(QWidget* widget)
    {
        for (auto* parent = widget->parentWidget(); parent != nullptr; parent = parent->parentWidget())
        {
            if (qobject_cast<slopkit::ui::widgets::Panel*>(parent) != nullptr)
            {
                return parent;
            }
        }
        return nullptr;
    }

    bool panel_has_title(QWidget* panel, const QString& title)
    {
        for (auto* label : panel->findChildren<QLabel*>())
        {
            if (label->text() == title)
            {
                return true;
            }
        }
        return false;
    }

    QCheckBox* checkbox_labelled(QWidget& root, const QString& text)
    {
        for (auto* box : root.findChildren<QCheckBox*>())
        {
            if (box->text() == text)
            {
                return box;
            }
        }
        return nullptr;
    }

    QPushButton* button_labelled(QWidget& root, const QString& text)
    {
        for (auto* button : root.findChildren<QPushButton*>())
        {
            if (button->text() == text)
            {
                return button;
            }
        }
        return nullptr;
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

TEST_CASE("the found-results model marks and groups static hits", "[ui]")
{
    application();

    slopkit::ui::models::FoundResultsModel model;

    slopkit::scan::ScanConfig config;
    config.value_type = slopkit::scan::ValueType::int32;

    slopkit::scan::ScanSnapshot snapshot;
    snapshot.hit_count = 3;
    snapshot.hits.push_back(slopkit::scan::ScanHit {0x1000, std::vector<std::byte> {std::byte {1}}, {}});
    snapshot.hits.push_back(slopkit::scan::ScanHit {0x5000000, std::vector<std::byte> {std::byte {5}}, {}});
    snapshot.hits.push_back(slopkit::scan::ScanHit {0x2000, std::vector<std::byte> {std::byte {2}}, {}});
    model.set_snapshot(snapshot, config);

    // No map yet: the plain ascending address order and no static hit.
    CHECK(model.hit_at(0)->address == 0x1000);
    CHECK(model.hit_at(1)->address == 0x2000);
    CHECK(model.hit_at(2)->address == 0x5000000);
    CHECK_FALSE(model.is_static(0x1000));

    // Ranges deliberately out of order; the model sorts them.
    model.set_module_ranges({
        slopkit::ui::models::AddressRange {0x2000, 0x3000},
         slopkit::ui::models::AddressRange {0x1000, 0x1800}
    });

    // Static hits group first in address order, the heap hit last.
    CHECK(model.hit_at(0)->address == 0x1000);
    CHECK(model.hit_at(1)->address == 0x2000);
    CHECK(model.hit_at(2)->address == 0x5000000);
    CHECK(model.is_static(0x1000));
    CHECK(model.is_static(0x17FF));
    CHECK_FALSE(model.is_static(0x1800)); // Half-open: base + size is outside.
    CHECK_FALSE(model.is_static(0x5000000));

    const QColor green = slopkit::ui::active_theme().success;
    CHECK(model.data(model.index(0, slopkit::ui::models::FoundResultsModel::address), Qt::ForegroundRole)
              .value<QBrush>()
              .color()
          == green);
    CHECK(model.data(model.index(1, slopkit::ui::models::FoundResultsModel::address), Qt::ForegroundRole)
              .value<QBrush>()
              .color()
          == green);
    CHECK_FALSE(
        model.data(model.index(2, slopkit::ui::models::FoundResultsModel::address), Qt::ForegroundRole).isValid());

    // The grouping is the primary key: statics stay on top when the address
    // column is sorted descending, now ordered descending within the group.
    model.sort(slopkit::ui::models::FoundResultsModel::address, Qt::DescendingOrder);
    CHECK(model.hit_at(0)->address == 0x2000);
    CHECK(model.hit_at(1)->address == 0x1000);
    CHECK(model.hit_at(2)->address == 0x5000000);

    // Dropping the map makes every hit non-static again.
    model.set_module_ranges({});
    CHECK_FALSE(model.is_static(0x1000));
    CHECK_FALSE(model.is_static(0x2000));
    CHECK(model.rowCount() == 3);
}

TEST_CASE("the found list keeps one result line", "[ui]")
{
    application();

    // A fresh panel already shows the pre-scan line, and it only ever has the one
    // label, so nothing can appear or disappear and shift the table.
    slopkit::scan::ScanEngine           idle_engine;
    slopkit::table::AddressTable        idle_table;
    slopkit::ui::panels::FoundListPanel idle_panel {idle_engine, idle_table};
    auto*                               idle_header = idle_panel.findChild<QLabel*>();
    REQUIRE(idle_header != nullptr);
    CHECK(idle_header->text() == QStringLiteral("Showing 0 of 0 results"));
    CHECK(idle_panel.findChildren<QLabel*>().size() == 1);

    // A capped page keeps the same line and appends the cap suffix instead of
    // adding a second status row.
    std::vector<std::byte> bytes(12, std::byte {0});
    for (std::size_t i = 0; i < 3; ++i)
    {
        const std::uint32_t value = 10;
        std::memcpy(bytes.data() + i * 4, &value, sizeof(value));
    }

    slopkit::scan::ScanEngine engine;
    engine.set_max_stored_hits(2);
    slopkit::scan::ScanConfig config;
    config.value = std::int64_t {10};
    engine.first_scan(config, slopkit::scan::make_buffer_source(bytes, 0x1000));
    for (int i = 0; i < 5000 && engine.is_running(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(engine.has_results());
    const auto snapshot = engine.snapshot();
    REQUIRE(snapshot.hits.size() == 2);
    REQUIRE(snapshot.hit_count == 3);
    REQUIRE(snapshot.truncated);

    slopkit::table::AddressTable        table;
    slopkit::ui::panels::FoundListPanel panel {engine, table};
    panel.refresh();

    auto* header = panel.findChild<QLabel*>();
    REQUIRE(header != nullptr);
    CHECK(header->text() == QStringLiteral("Showing 2 of 3 results (result cap reached)"));
    CHECK(panel.findChildren<QLabel*>().size() == 1);
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
                             QStringLiteral("Save Table As..."),
                             QStringLiteral("Quit")});

    // The toolbar is gone; the file commands live only in the menus now.
    CHECK(window.findChild<QToolBar*>(QStringLiteral("main_toolbar")) == nullptr);

    // The Edit menu is now the only route to the settings dialog.
    CHECK(menus[1]->menu()->actions().last()->text() == QStringLiteral("Settings..."));

    // Ctrl+T / Ctrl+O / Ctrl+S / Ctrl+Shift+S are bound to the file commands.
    const QList<QAction*> file_actions = menus[0]->menu()->actions();
    REQUIRE(file_actions.size() == 6);
    CHECK(file_actions[0]->shortcut() == QKeySequence(QStringLiteral("Ctrl+T")));
    CHECK(file_actions[1]->shortcut() == QKeySequence(QKeySequence::Open));
    CHECK(file_actions[2]->shortcut() == QKeySequence(QKeySequence::Save));
    CHECK(file_actions[3]->shortcut() == QKeySequence(QKeySequence::SaveAs));

    // The Table menu reuses the very same save-as action, so its shortcut is not
    // registered twice.
    CHECK(menus[2]->menu()->actions().last() == file_actions[3]);

    // The status bar carries only the detached-process label; no progress bar
    // lives there any more.
    auto* process_label = window.statusBar()->findChild<QLabel*>();
    REQUIRE(process_label != nullptr);
    CHECK(process_label->text() == QStringLiteral("No Process Selected"));
    CHECK(window.statusBar()->findChild<QProgressBar*>() == nullptr);

    // The central widget is a column: the single full-width progress bar sits
    // above the split zones.
    auto* column = window.centralWidget();
    REQUIRE(column != nullptr);
    auto* progress = column->findChild<QProgressBar*>();
    REQUIRE(progress != nullptr);
    CHECK(progress->value() == 0);

    // Two split zones inside the middle zone, the address list below them.
    QSplitter* vertical = nullptr;
    for (auto* splitter : column->findChildren<QSplitter*>())
    {
        if (splitter->orientation() == Qt::Vertical)
        {
            vertical = splitter;
        }
    }
    REQUIRE(vertical != nullptr);
    REQUIRE(vertical->count() == 2);

    auto* middle = qobject_cast<QSplitter*>(vertical->widget(0));
    REQUIRE(middle != nullptr);
    CHECK(middle->orientation() == Qt::Horizontal);
    CHECK(middle->count() == 2);

    // The window opens at the compact default size.
    CHECK(window.size() == QSize(748, 768));

    // The middle zone holds the live found list and the scanner controls.
    auto* found_list = window.findChild<slopkit::ui::panels::FoundListPanel*>();
    REQUIRE(found_list != nullptr);
    auto* scanner = window.findChild<slopkit::ui::panels::ScannerPanel*>();
    REQUIRE(scanner != nullptr);

    // The scanner options are a plain panel, not a collapsible section: the old
    // CollapsibleSection used a QToolButton toggle, so none must remain.
    bool has_hex_checkbox = false;
    for (auto* box : scanner->findChildren<QCheckBox*>())
    {
        has_hex_checkbox = has_hex_checkbox || box->text() == QStringLiteral("Hex");
    }
    CHECK(has_hex_checkbox);
    CHECK(scanner->findChild<QToolButton*>() == nullptr);

    // The decorative "advanced" checkboxes are gone, along with the whole
    // "Extra options" section and its placeholder options.
    CHECK(checkbox_labelled(*scanner, QStringLiteral("Lua formula")) == nullptr);
    CHECK(checkbox_labelled(*scanner, QStringLiteral("Enable Speedhack")) == nullptr);
    CHECK(checkbox_labelled(*scanner, QStringLiteral("Not")) == nullptr);
    CHECK(checkbox_labelled(*scanner, QStringLiteral("Unrandomizer")) == nullptr);
    CHECK_FALSE(panel_has_title(scanner, QStringLiteral("Extra options")));

    // The surviving options live inside the Memory Scan Options panel.
    auto* writable_check = checkbox_labelled(*scanner, QStringLiteral("Writable"));
    REQUIRE(writable_check != nullptr);
    auto* options_panel = ancestor_panel(writable_check);
    REQUIRE(options_panel != nullptr);
    CHECK(panel_has_title(options_panel, QStringLiteral("Memory Scan Options")));

    auto* found_header = found_list->findChild<QLabel*>();
    REQUIRE(found_header != nullptr);
    CHECK(found_header->text() == QStringLiteral("Showing 0 of 0 results"));

    // The address list lost its decorative footer buttons.
    auto* address_list = window.findChild<slopkit::ui::panels::AddressListPanel*>();
    REQUIRE(address_list != nullptr);
    for (auto* button : address_list->findChildren<QPushButton*>())
    {
        CHECK(button->text() != QStringLiteral("Advanced Options"));
        CHECK(button->text() != QStringLiteral("Table Extras"));
    }

    // The Add Address button lives at the bottom-right of the scanner panel,
    // not in the found list or the address list.
    CHECK(button_labelled(*address_list, QStringLiteral("Add Address Manually")) == nullptr);
    CHECK(button_labelled(*found_list, QStringLiteral("Add Address Manually")) == nullptr);
    CHECK(button_labelled(*scanner, QStringLiteral("Add Address Manually")) != nullptr);

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

TEST_CASE("the scanner range is pre-filled with the padded defaults", "[ui]")
{
    application();

    UiFakeAccess access;

    slopkit::process::RegionInfo low;
    low.start    = 0x1000;
    low.end      = 0x3000;
    low.readable = true;

    access.regions = {low};

    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    // No attach or map job yet: the boxes already hold the whole-address-space
    // range, independent of the target's memory map.
    slopkit::ui::panels::ScannerPanel panel {worker, target};

    auto* start = address_field(panel, "Start address");
    auto* stop  = address_field(panel, "Stop address");
    auto* combo = range_combo(panel);
    REQUIRE(start != nullptr);
    REQUIRE(stop != nullptr);
    REQUIRE(combo != nullptr);

    CHECK(start->text() == QStringLiteral("0x0000000000000000"));
    CHECK(stop->text() == QStringLiteral("0x00007FFFFFFFFFFF"));
    CHECK_FALSE(combo->isEnabled());
}

TEST_CASE("starting a scan hands the whole address space range to the engine", "[ui]")
{
    application();

    UiFakeAccess                 access;
    slopkit::process::RegionInfo region;
    region.start    = 0x1000;
    region.end      = 0x3000;
    region.readable = true;
    access.regions  = {region};

    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    attach_app_session(worker);

    slopkit::ui::panels::ScannerPanel panel {worker, target};

    auto* start = address_field(panel, "Start address");
    REQUIRE(start != nullptr);

    // A first scan needs a value to search for; the range is what we assert.
    auto* value = address_field(panel, "Value");
    REQUIRE(value != nullptr);
    value->setText(QStringLiteral("10"));

    QPushButton* scan_button = nullptr;
    for (auto* button : panel.findChildren<QPushButton*>())
    {
        if (button->text() == QStringLiteral("First Scan"))
        {
            scan_button = button;
            break;
        }
    }
    REQUIRE(scan_button != nullptr);

    // Wait until the map and the session handoff land: a refresh then enables
    // scanning.
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        panel.refresh();
                        return scan_button->isEnabled();
                    }));
    scan_button->click();

    const auto config = panel.engine().config();
    CHECK(config.filter.start == 0);
    CHECK(config.filter.stop == slopkit::scan::kMaxUserAddress);
}

TEST_CASE("New Scan clears the results and returns the panel to its pre-scan state", "[ui]")
{
    application();

    UiFakeAccess                 access;
    slopkit::process::RegionInfo region;
    region.start    = 0x1000;
    region.end      = 0x3000;
    region.readable = true;
    region.writable = true;
    access.regions  = {region};

    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    attach_app_session(worker);

    slopkit::ui::panels::ScannerPanel panel {worker, target};

    auto* value = address_field(panel, "Value");
    REQUIRE(value != nullptr);
    value->setText(QStringLiteral("10"));

    auto* scan_button = button_labelled(panel, QStringLiteral("First Scan"));
    auto* undo_button = button_labelled(panel, QStringLiteral("Undo Scan"));
    auto* next_button = button_labelled(panel, QStringLiteral("Next Scan"));
    REQUIRE(scan_button != nullptr);
    REQUIRE(undo_button != nullptr);
    REQUIRE(next_button != nullptr);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        panel.refresh();
                        return scan_button->isEnabled();
                    }));

    // The first scan runs to completion and leaves a result set behind.
    scan_button->click();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        panel.refresh();
                        return panel.engine().has_results() && !panel.engine().is_running();
                    }));
    panel.refresh();
    REQUIRE(scan_button->text() == QStringLiteral("New Scan"));
    CHECK(undo_button->isEnabled());

    // New Scan drops the result set instead of starting another scan.
    scan_button->click();
    panel.refresh();

    CHECK_FALSE(panel.engine().has_results());
    CHECK(scan_button->text() == QStringLiteral("First Scan"));
    CHECK_FALSE(undo_button->isEnabled());
    CHECK_FALSE(next_button->isEnabled());
    CHECK(panel.progress_percent() == 0);

    // The found list over the same engine is empty again.
    slopkit::table::AddressTable        addresses;
    slopkit::ui::panels::FoundListPanel found_list {panel.engine(), addresses};
    found_list.refresh();
    auto* view = found_list.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    REQUIRE(view->model() != nullptr);
    CHECK(view->model()->rowCount() == 0);
}

TEST_CASE("the scanner range always shows the padded whole-address-space defaults", "[ui]")
{
    application();

    // Waits for the memory map, then checks the boxes hold the padded default
    // range whatever the mapped pages are.
    const auto expects_defaults = [](UiFakeAccess& access)
    {
        slopkit::process::AccessWorker   worker {access};
        slopkit::process::AttachedTarget target = fake_target();

        attach_app_session(worker);

        slopkit::ui::panels::ScannerPanel panel {worker, target};

        auto* start = address_field(panel, "Start address");
        auto* stop  = address_field(panel, "Stop address");
        auto* combo = range_combo(panel);
        REQUIRE(start != nullptr);
        REQUIRE(stop != nullptr);
        REQUIRE(combo != nullptr);

        REQUIRE(pump_ui(worker,
                        [&]
                        {
                            return combo->isEnabled();
                        }));
        CHECK(start->text() == QStringLiteral("0x0000000000000000"));
        CHECK(stop->text() == QStringLiteral("0x00007FFFFFFFFFFF"));
    };

    SECTION("an empty map")
    {
        UiFakeAccess access; // No regions reported.
        expects_defaults(access);
    }

    SECTION("a low non-readable region")
    {
        UiFakeAccess access;

        slopkit::process::RegionInfo guard;
        guard.start    = 0x1000;
        guard.end      = 0x2000;
        guard.readable = false;

        slopkit::process::RegionInfo readable;
        readable.start    = 0x3000;
        readable.end      = 0x6000;
        readable.readable = true;

        access.regions = {readable, guard};
        expects_defaults(access);
    }

    SECTION("a lone non-readable region")
    {
        UiFakeAccess access;

        slopkit::process::RegionInfo only;
        only.start     = 0x2000;
        only.end       = 0x6000;
        only.readable  = false;
        access.regions = {only};

        expects_defaults(access);
    }
}

TEST_CASE("the scan range dropdown lists file-backed modules and narrows the range", "[ui]")
{
    application();

    UiFakeAccess access;

    slopkit::process::RegionInfo region;
    region.start    = 0x1000;
    region.end      = 0x3000;
    region.readable = true;
    access.regions  = {region};

    slopkit::process::ModuleInfo high;
    high.base = 0x2000;
    high.size = 0x100;
    high.kind = slopkit::process::ModuleKind::elf;
    high.name = "high";
    high.path = "/opt/high";

    slopkit::process::ModuleInfo low;
    low.base = 0x1000;
    low.size = 0x800;
    low.kind = slopkit::process::ModuleKind::elf;
    low.name = "low";
    low.path = "/opt/low";

    slopkit::process::ModuleInfo anon;
    anon.base = 0x4000;
    anon.size = 0x1000;
    anon.kind = slopkit::process::ModuleKind::anonymous;
    anon.name = "[anon]";

    // Deliberately out of base order to prove the dropdown sorts.
    access.modules = {high, anon, low};

    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    attach_app_session(worker);

    slopkit::ui::panels::ScannerPanel panel {worker, target};

    auto* start = address_field(panel, "Start address");
    auto* stop  = address_field(panel, "Stop address");
    auto* combo = range_combo(panel);
    REQUIRE(start != nullptr);
    REQUIRE(stop != nullptr);
    REQUIRE(combo != nullptr);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return combo->isEnabled();
                    }));

    // Item 0 is the whole process, then the file-backed modules by base; the
    // anonymous mapping stays out of the list.
    REQUIRE(combo->count() == 3);
    CHECK(combo->itemText(0) == QStringLiteral("All memory"));
    CHECK(combo->itemText(1) == QStringLiteral("low"));
    CHECK(combo->itemText(2) == QStringLiteral("high"));
    for (int i = 0; i < combo->count(); ++i)
    {
        CHECK_FALSE(combo->itemText(i).contains(QStringLiteral("0x")));
    }
    // The range moved into the tooltips instead of the entry text.
    CHECK(combo->itemData(0, Qt::ToolTipRole).toString() == QStringLiteral("0x0000000000000000-0x00007FFFFFFFFFFF"));
    CHECK(combo->itemData(1, Qt::ToolTipRole).toString() == QStringLiteral("/opt/low"));

    // Selecting a module narrows the range to its base and size.
    combo->setCurrentIndex(1);
    CHECK(start->text() == QStringLiteral("0x0000000000001000"));
    CHECK(stop->text() == QStringLiteral("0x0000000000001800"));

    combo->setCurrentIndex(2);
    CHECK(start->text() == QStringLiteral("0x0000000000002000"));
    CHECK(stop->text() == QStringLiteral("0x0000000000002100"));

    // Back to the whole process.
    combo->setCurrentIndex(0);
    CHECK(start->text() == QStringLiteral("0x0000000000000000"));
    CHECK(stop->text() == QStringLiteral("0x00007FFFFFFFFFFF"));
}

TEST_CASE("a memory map applied to the scanner panel reaches the found list", "[ui]")
{
    application();

    UiFakeAccess access;

    slopkit::process::RegionInfo region;
    region.start    = 0x1000;
    region.end      = 0x3000;
    region.readable = true;
    access.regions  = {region};

    slopkit::process::ModuleInfo image;
    image.base     = 0x1000;
    image.size     = 0x2000;
    image.kind     = slopkit::process::ModuleKind::elf;
    image.name     = "app";
    image.path     = "/opt/app";
    access.modules = {image};

    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    attach_app_session(worker);

    slopkit::ui::panels::ScannerPanel panel {worker, target};

    slopkit::table::AddressTable        addresses;
    slopkit::ui::panels::FoundListPanel found_list {panel.engine(), addresses};
    QObject::connect(&panel,
                     &slopkit::ui::panels::ScannerPanel::memoryMapApplied,
                     &found_list,
                     &slopkit::ui::panels::FoundListPanel::set_modules);

    auto* combo = range_combo(panel);
    REQUIRE(combo != nullptr);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return combo->isEnabled();
                    }));

    auto* view = found_list.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    auto* model = dynamic_cast<slopkit::ui::models::FoundResultsModel*>(view->model());
    REQUIRE(model != nullptr);

    // The module image span arrived through the signal: inside is static, the
    // half-open end and an unrelated address are not.
    CHECK(model->is_static(0x1000));
    CHECK(model->is_static(0x2FFF));
    CHECK_FALSE(model->is_static(0x3000));
    CHECK_FALSE(model->is_static(0x5000000));
}

TEST_CASE("a long module list scrolls inside the scan-range dropdown", "[ui]")
{
    application();

    slopkit::ui::widgets::ScrollingComboBox combo;
    combo.resize(320, 30);
    for (int i = 0; i < 40; ++i)
    {
        combo.addItem(
            QStringLiteral("libmodule%1.so.6  0x7F0%2-0x7F1%2").arg(i).arg(i * 0x1000, 8, 16, QLatin1Char('0')));
    }
    combo.show();
    QCoreApplication::processEvents();

    combo.showPopup();
    QCoreApplication::processEvents();

    QAbstractItemView* view = combo.view();
    REQUIRE(view != nullptr);
    CHECK(view->window() != &combo);

    const int row_height = view->sizeHintForRow(0);
    REQUIRE(row_height > 0);

    // The popup shows at most the visible rows and scrolls the rest instead of
    // growing over the whole screen.
    CHECK(view->height() <= row_height * combo.maxVisibleItems());
    CHECK(view->verticalScrollBar()->maximum() > 0);

    combo.hidePopup();
}

TEST_CASE("manual edits to the scan range survive refresh cycles", "[ui]")
{
    application();

    UiFakeAccess                 access;
    slopkit::process::RegionInfo region;
    region.start    = 0x1000;
    region.end      = 0x3000;
    region.readable = true;
    access.regions  = {region};

    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    attach_app_session(worker);

    slopkit::ui::panels::ScannerPanel panel {worker, target};

    auto* start = address_field(panel, "Start address");
    auto* combo = range_combo(panel);
    REQUIRE(start != nullptr);
    REQUIRE(combo != nullptr);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return combo->isEnabled();
                    }));
    CHECK(start->text() == QStringLiteral("0x0000000000000000"));

    // A hand-typed value must not be rewritten by later ticks.
    start->setText(QStringLiteral("0x2000"));
    for (int tick = 0; tick < 5; ++tick)
    {
        panel.refresh();
        worker.drain();
        QCoreApplication::processEvents();
    }
    CHECK(start->text() == QStringLiteral("0x2000"));
}

TEST_CASE("detaching resets the scan range and re-attaching repopulates it", "[ui]")
{
    application();

    UiFakeAccess                 access;
    slopkit::process::RegionInfo region;
    region.start    = 0x1000;
    region.end      = 0x3000;
    region.readable = true;
    access.regions  = {region};

    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    attach_app_session(worker);

    slopkit::ui::panels::ScannerPanel panel {worker, target};

    auto* start = address_field(panel, "Start address");
    auto* stop  = address_field(panel, "Stop address");
    auto* combo = range_combo(panel);
    REQUIRE(start != nullptr);
    REQUIRE(stop != nullptr);
    REQUIRE(combo != nullptr);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return combo->isEnabled();
                    }));
    CHECK(start->text() == QStringLiteral("0x0000000000000000"));

    // Detach: the boxes return to the padded defaults and the dropdown resets
    // to a disabled entry.
    target.clear();
    panel.refresh();
    CHECK(start->text() == QStringLiteral("0x0000000000000000"));
    CHECK(stop->text() == QStringLiteral("0x00007FFFFFFFFFFF"));
    CHECK_FALSE(combo->isEnabled());
    REQUIRE(combo->count() == 1);
    CHECK(combo->itemText(0) == QStringLiteral("All memory"));

    // Re-attach: the map is requested again and the defaults are restored.
    target = fake_target();
    panel.refresh();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return combo->isEnabled();
                    }));
    CHECK(start->text() == QStringLiteral("0x0000000000000000"));
    CHECK(stop->text() == QStringLiteral("0x00007FFFFFFFFFFF"));
}

TEST_CASE("a stale memory map result does not overwrite the range", "[ui]")
{
    application();

    UiFakeAccess                 access;
    slopkit::process::RegionInfo region;
    region.start    = 0x1000;
    region.end      = 0x3000;
    region.readable = true;
    access.regions  = {region};

    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    // Build the panel before the app-wide attach, so the handoff drains without
    // a map request: the map is only asked for on an explicit refresh below.
    slopkit::ui::panels::ScannerPanel panel {worker, target};

    auto* start = address_field(panel, "Start address");
    auto* combo = range_combo(panel);
    REQUIRE(start != nullptr);
    REQUIRE(combo != nullptr);

    attach_app_session(worker);
    panel.refresh(); // Submits the map job for pid 42 (shows "Loading…").

    // Let the job finish and queue its completion, then change the target
    // before the completion is drained: it must be dropped.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    target.pid = 99;
    worker.drain();
    QCoreApplication::processEvents();

    CHECK(start->text() == QStringLiteral("0x0000000000000000"));
    CHECK_FALSE(combo->isEnabled());
    REQUIRE(combo->count() == 1);
    CHECK(combo->itemText(0) == QStringLiteral("Loading…"));
}

TEST_CASE("the scanner resolves the main module entry point", "[ui]")
{
    application();

    const auto address_for = [](std::uint64_t low_entry)
    {
        UiFakeAccess access;

        slopkit::process::ModuleInfo high;
        high.base  = 0x2000;
        high.size  = 0x100;
        high.entry = 0x2100;
        high.kind  = slopkit::process::ModuleKind::elf;
        high.name  = "high";
        high.path  = "/opt/high";

        slopkit::process::ModuleInfo low;
        low.base  = 0x1000;
        low.size  = 0x800;
        low.entry = low_entry;
        low.kind  = slopkit::process::ModuleKind::elf;
        low.name  = "low";
        low.path  = "/opt/low";

        slopkit::process::ModuleInfo anon;
        anon.base = 0x4000;
        anon.size = 0x1000;
        anon.kind = slopkit::process::ModuleKind::anonymous;
        anon.name = "[anon]";

        // Deliberately out of base order to prove the resolution sorts.
        access.modules = {high, anon, low};

        slopkit::process::AccessWorker   worker {access};
        slopkit::process::AttachedTarget target = fake_target();
        attach_app_session(worker);

        slopkit::ui::panels::ScannerPanel panel {worker, target};
        pump_ui(worker,
                [&]
                {
                    return panel.main_module_address() != 0;
                });
        return panel.main_module_address();
    };

    // A known entry point wins over the module base...
    CHECK(address_for(0x1040) == 0x1040);

    // ...and a zero entry falls back to the lowest-base module.
    CHECK(address_for(0) == 0x1000);

    // Without a memory map there is no address at all.
    UiFakeAccess                      no_map_access;
    slopkit::process::AccessWorker    no_map_worker {no_map_access};
    slopkit::process::AttachedTarget  invalid_target;
    slopkit::ui::panels::ScannerPanel no_map_panel {no_map_worker, invalid_target};
    CHECK(no_map_panel.main_module_address() == 0);
}

TEST_CASE("the found-results entry row drives the viewer", "[ui]")
{
    application();

    slopkit::scan::ScanEngine           engine;
    slopkit::table::AddressTable        table;
    slopkit::ui::panels::FoundListPanel panel {engine, table};

    auto* memory_view = button_labelled(panel, QStringLiteral("Memory View"));
    REQUIRE(memory_view != nullptr);
    // The Add Address button moved to the scanner panel's bottom-right.
    CHECK(button_labelled(panel, QStringLiteral("Add Address Manually")) == nullptr);

    // The view button waits for an attached target.
    CHECK_FALSE(memory_view->isEnabled());

    panel.resize(600, 400);
    panel.show();
    QCoreApplication::processEvents();

    // The button sits in the row directly below the hits table.
    auto* hits = panel.findChild<QTableView*>();
    REQUIRE(hits != nullptr);
    CHECK(memory_view->y() > hits->y());

    bool view_requested = false;
    QObject::connect(&panel,
                     &slopkit::ui::panels::FoundListPanel::memoryViewRequested,
                     &panel,
                     [&]
                     {
                         view_requested = true;
                     });

    panel.set_target_attached(true);
    CHECK(memory_view->isEnabled());
    memory_view->click();
    CHECK(view_requested);

    panel.set_target_attached(false);
    CHECK_FALSE(memory_view->isEnabled());
}

TEST_CASE("the found-list entry row opens the viewer at the main module entry", "[ui]")
{
    application();

    UiFakeAccess access;

    slopkit::process::ModuleInfo image;
    image.base     = 0x1000;
    image.size     = 0x800;
    image.entry    = 0x1040;
    image.kind     = slopkit::process::ModuleKind::elf;
    image.name     = "low";
    image.path     = "/opt/low";
    access.modules = {image};

    slopkit::plugin::PluginHost      host;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    slopkit::ui::MainWindow          window {worker, target, host};

    attach_app_session(worker);

    auto* found_list = window.findChild<slopkit::ui::panels::FoundListPanel*>();
    REQUIRE(found_list != nullptr);
    auto* scanner = window.findChild<slopkit::ui::panels::ScannerPanel*>();
    REQUIRE(scanner != nullptr);

    auto* memory_view = button_labelled(*found_list, QStringLiteral("Memory View"));
    REQUIRE(memory_view != nullptr);
    auto* add_address = button_labelled(*scanner, QStringLiteral("Add Address Manually"));
    REQUIRE(add_address != nullptr);
    CHECK(scanner->isAncestorOf(add_address));
    CHECK(button_labelled(*found_list, QStringLiteral("Add Address Manually")) == nullptr);

    // The window enables the view button once the target is attached and the
    // main module's map has landed.
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return memory_view->isEnabled() && scanner->main_module_address() != 0;
                    }));

    window.show();
    QCoreApplication::processEvents();

    // The two buttons share one window-wide row: Memory View on the left and Add
    // Address Manually against the window's right edge.
    const QPoint memory_view_pos = memory_view->mapTo(&window, QPoint(0, 0));
    const QPoint add_address_pos = add_address->mapTo(&window, QPoint(0, 0));
    CHECK(memory_view_pos.y() == add_address_pos.y());
    CHECK(memory_view_pos.x() < add_address_pos.x());
    CHECK(window.width() - add_address->mapTo(&window, add_address->rect().topRight()).x() <= 24);

    auto* viewer = window.findChild<slopkit::ui::dialogs::MemoryViewerDialog*>();
    REQUIRE(viewer != nullptr);
    auto* address_edit = viewer->findChild<QLineEdit*>();
    REQUIRE(address_edit != nullptr);

    memory_view->click();
    CHECK(address_edit->text() == QStringLiteral("0x1040"));

    // The add button reaches the same non-modal dialog as the menu action.
    auto* add_dialog = window.findChild<slopkit::ui::dialogs::AddAddressDialog*>();
    REQUIRE(add_dialog != nullptr);
    CHECK_FALSE(add_dialog->isVisible());
    add_address->click();
    CHECK(add_dialog->isVisible());
}
