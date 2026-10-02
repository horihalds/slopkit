#include <catch2/catch.hpp>

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QLabel>
#include <QList>
#include <QMenu>
#include <QMenuBar>
#include <QPalette>
#include <QProgressBar>
#include <QSplitter>
#include <QStatusBar>
#include <QString>
#include <QToolBar>

#include "plugin/plugin_host.hpp"
#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/plugin_access.hpp"
#include "ui/components/widgets.hpp"
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

    // The toolbar carries the four shortcuts; the labels drop the ellipsis.
    auto* toolbar = window.findChild<QToolBar*>(QStringLiteral("main_toolbar"));
    REQUIRE(toolbar != nullptr);
    QList<QString> toolbar_labels;
    for (QAction* action : toolbar->actions())
    {
        // The toolbar also carries the spacer widget, which has no label.
        if (!action->isSeparator() && !action->iconText().isEmpty())
        {
            toolbar_labels.append(action->iconText());
        }
    }
    CHECK(toolbar_labels
          == QList<QString> {QStringLiteral("Open Process"),
                             QStringLiteral("Open Table"),
                             QStringLiteral("Save Table"),
                             QStringLiteral("Settings")});

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
