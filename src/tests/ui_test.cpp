#include <catch2/catch.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPalette>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QSplitter>
#include <QStatusBar>
#include <QString>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QToolTip>

#include "core/log.hpp"
#include "plugin/plugin_host.hpp"
#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/plugin_access.hpp"
#include "process/types.hpp"
#include "scan/source.hpp"
#include "ui/address_format.hpp"
#include "ui/components/widgets.hpp"
#include "ui/dialogs/add_address.hpp"
#include "ui/dialogs/log.hpp"
#include "ui/dialogs/memory_viewer.hpp"
#include "ui/dialogs/process_list.hpp"
#include "ui/dialogs/settings.hpp"
#include "ui/main_window.hpp"
#include "ui/models/address_table_model.hpp"
#include "ui/models/found_results_model.hpp"
#include "ui/panels/address_list_panel.hpp"
#include "ui/panels/found_list_panel.hpp"
#include "ui/panels/scanner_panel.hpp"
#include "ui/settings.hpp"
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

    // A fresh settings file under the project scratch directory, so the tests
    // never touch the developer's real configuration.
    QString scratch_settings_file(std::string_view name)
    {
        const auto      directory = std::filesystem::path(SLOPKIT_TMP_DIR) / "ui_test";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        const auto path = directory / name;
        std::filesystem::remove(path, error);
        return QString::fromStdString(path.string());
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

    QRadioButton* radio_labelled(QWidget& root, const QString& text)
    {
        for (auto* button : root.findChildren<QRadioButton*>())
        {
            if (button->text() == text)
            {
                return button;
            }
        }
        return nullptr;
    }

    // A file-backed module image fixture for the module-span tests.
    slopkit::process::ModuleInfo module_image(std::string name, std::uint64_t base, std::uint64_t size)
    {
        slopkit::process::ModuleInfo module;
        module.kind = slopkit::process::ModuleKind::elf;
        module.name = std::move(name);
        module.base = base;
        module.size = size;
        return module;
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

TEST_CASE("the module spans keep file-backed images sorted by base", "[ui]")
{
    slopkit::ui::ModuleSpans spans;
    CHECK(spans.empty());
    CHECK(spans.containing(0x1000) == nullptr);
    CHECK(spans.find_by_name("low") == nullptr);

    std::vector<slopkit::process::ModuleInfo> modules;
    modules.push_back(module_image("low", 0x2000, 0x1000));
    modules.push_back(module_image("high", 0x5000, 0x100));
    // Not file-backed: dropped from the map.
    slopkit::process::ModuleInfo anonymous;
    anonymous.base = 0x3000;
    anonymous.size = 0x1000;
    modules.push_back(anonymous);
    // Deliberately out of order; the map sorts by base.
    modules.push_back(module_image("first", 0x1000, 0x800));

    spans.set_modules(modules);

    CHECK_FALSE(spans.empty());
    REQUIRE(spans.containing(0x1000) != nullptr);
    CHECK(spans.containing(0x1000)->name == "first");
    CHECK(spans.containing(0x17FF)->name == "first");
    CHECK(spans.containing(0x1800) == nullptr); // Half-open: base + size is outside.
    CHECK(spans.containing(0x2FFF)->name == "low");
    CHECK(spans.containing(0x3000) == nullptr); // The anonymous mapping never labels an address.
    CHECK(spans.containing(0x0FFF) == nullptr);

    // The name lookup is case-insensitive.
    REQUIRE(spans.find_by_name("LOW") != nullptr);
    CHECK(spans.find_by_name("High")->base == 0x5000);
    CHECK(spans.find_by_name("missing") == nullptr);

    spans.clear();
    CHECK(spans.empty());
}

TEST_CASE("the module spans mark the main image", "[ui]")
{
    slopkit::ui::ModuleSpans spans;
    CHECK(spans.main() == nullptr);

    std::vector<slopkit::process::ModuleInfo> modules;
    modules.push_back(module_image("low", 0x2000, 0x1000));
    modules.push_back(module_image("lib", 0x8000, 0x100));
    // The flagged main image is not the lowest-based one.
    slopkit::process::ModuleInfo game = module_image("game", 0x5000, 0x1000);
    game.is_main                      = true;
    modules.push_back(game);

    spans.set_modules(modules);

    REQUIRE(spans.main() != nullptr);
    CHECK(spans.main()->name == "game");
    CHECK(spans.main()->base == 0x5000);
    CHECK(spans.main()->is_main);
    CHECK(spans.containing(0x5000)->is_main);
    CHECK_FALSE(spans.containing(0x2000)->is_main);
    CHECK_FALSE(spans.containing(0x8000)->is_main);

    // Without a flagged image the lowest-based image becomes the main one.
    const std::vector<slopkit::process::ModuleInfo> unflagged {module_image("high", 0x5000, 0x100),
                                                               module_image("low", 0x2000, 0x1000)};
    spans.set_modules(unflagged);
    REQUIRE(spans.main() != nullptr);
    CHECK(spans.main()->name == "low");
    CHECK(spans.main()->is_main);

    spans.clear();
    CHECK(spans.main() == nullptr);
}

TEST_CASE("a module without a name is labelled from its path", "[ui]")
{
    slopkit::process::ModuleInfo module = module_image("", 0x4000, 0x100);
    module.path                         = "/usr/lib/libc.so.6";
    const std::vector<slopkit::process::ModuleInfo> modules {module};

    slopkit::ui::ModuleSpans spans;
    spans.set_modules(modules);
    REQUIRE(spans.containing(0x4000) != nullptr);
    CHECK(spans.containing(0x4000)->name == "libc.so.6");
    CHECK(slopkit::ui::format_module_relative(*spans.containing(0x4000), 0x4000) == QStringLiteral("libc.so.6+0"));
}

TEST_CASE("module-relative addresses render as name+HEX", "[ui]")
{
    const slopkit::ui::ModuleSpan span {"app", 0x1000, 0x2000};

    CHECK(slopkit::ui::format_module_relative(span, 0x1000) == QStringLiteral("app+0"));
    CHECK(slopkit::ui::format_module_relative(span, 0x1040) == QStringLiteral("app+40"));
    CHECK(slopkit::ui::format_module_relative(span, 0x1A2B) == QStringLiteral("app+A2B"));
    // No 0x prefix and no leading zeros.
    CHECK_FALSE(slopkit::ui::format_module_relative(span, 0x1040).contains(QStringLiteral("0x")));
}

TEST_CASE("absolute addresses render as 0xHEX", "[ui]")
{
    CHECK(slopkit::ui::format_absolute(0) == QStringLiteral("0x0"));
    CHECK(slopkit::ui::format_absolute(0x1040) == QStringLiteral("0x1040"));
    CHECK(slopkit::ui::format_absolute(0x5000000) == QStringLiteral("0x5000000"));
    // Upper-case hex, no leading zeros.
    CHECK(slopkit::ui::format_absolute(0xabcdef) == QStringLiteral("0xABCDEF"));
}

TEST_CASE("module-relative text falls back in absolute mode and outside spans", "[ui]")
{
    const std::vector<slopkit::process::ModuleInfo> modules {module_image("app", 0x1000, 0x1000)};
    slopkit::ui::ModuleSpans                        spans;
    spans.set_modules(modules);

    const auto relative = slopkit::ui::module_relative_text(slopkit::ui::AddressMode::module_relative, spans, 0x1040);
    REQUIRE(relative.has_value());
    CHECK(*relative == QStringLiteral("app+40"));

    // Outside every span there is no module-relative text, whatever the mode.
    CHECK_FALSE(
        slopkit::ui::module_relative_text(slopkit::ui::AddressMode::module_relative, spans, 0x5000000).has_value());
    // The absolute mode never produces module-relative text.
    CHECK_FALSE(slopkit::ui::module_relative_text(slopkit::ui::AddressMode::absolute, spans, 0x1040).has_value());

    slopkit::ui::ModuleSpans empty;
    CHECK_FALSE(
        slopkit::ui::module_relative_text(slopkit::ui::AddressMode::module_relative, empty, 0x1040).has_value());
}

TEST_CASE("address text parses both absolute and module-relative forms", "[ui]")
{
    const std::vector<slopkit::process::ModuleInfo> modules {module_image("low", 0x1000, 0x1000)};
    slopkit::ui::ModuleSpans                        spans;
    spans.set_modules(modules);

    // Absolute forms still go through scan::parse_address.
    CHECK(slopkit::ui::parse_address_text("0x1040", spans) == std::optional<std::uint64_t> {0x1040});
    CHECK(slopkit::ui::parse_address_text("1040", spans) == std::optional<std::uint64_t> {1040});

    // module+RVA: case-insensitive name, optional 0x on the RVA.
    CHECK(slopkit::ui::parse_address_text("LOW+40", spans) == std::optional<std::uint64_t> {0x1040});
    CHECK(slopkit::ui::parse_address_text("low+0x40", spans) == std::optional<std::uint64_t> {0x1040});
    CHECK(slopkit::ui::parse_address_text("low+0", spans) == std::optional<std::uint64_t> {0x1000});

    // An unknown module or an unparsable RVA yields nothing.
    CHECK_FALSE(slopkit::ui::parse_address_text("missing+40", spans).has_value());
    CHECK_FALSE(slopkit::ui::parse_address_text("low+bogus", spans).has_value());
    CHECK_FALSE(slopkit::ui::parse_address_text("not an address", spans).has_value());
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

TEST_CASE("the found-results model renders the clipboard texts", "[ui]")
{
    application();

    slopkit::ui::models::FoundResultsModel model;

    slopkit::scan::ScanConfig config;
    config.value_type = slopkit::scan::ValueType::int32;

    slopkit::scan::ScanSnapshot snapshot;
    snapshot.hit_count = 2;
    snapshot.hits.push_back(slopkit::scan::ScanHit {
        0x1040, std::vector<std::byte> {std::byte {10}, std::byte {0}, std::byte {0}, std::byte {0}},
          {}
    });
    snapshot.hits.push_back(slopkit::scan::ScanHit {
        0x7F3A1B2C, std::vector<std::byte> {std::byte {100}, std::byte {0}, std::byte {0}, std::byte {0}},
          {}
    });
    model.set_snapshot(snapshot, config);

    // With no module map every hit copies as an absolute address.
    CHECK(model.copy_text(0, slopkit::ui::models::CopyFormat::module_relative) == QStringLiteral("0x1040"));

    model.set_modules({module_image("app", 0x1000, 0x1000)});

    // A static hit inside `app`.
    CHECK(model.copy_text(0, slopkit::ui::models::CopyFormat::module_relative) == QStringLiteral("app+40"));
    CHECK(model.copy_text(0, slopkit::ui::models::CopyFormat::absolute) == QStringLiteral("0x1040"));
    CHECK(model.copy_text(0, slopkit::ui::models::CopyFormat::address_and_value) == QStringLiteral("app+40: 10"));

    // A dynamic hit falls back to the absolute form for module + RVA.
    CHECK(model.copy_text(1, slopkit::ui::models::CopyFormat::module_relative) == QStringLiteral("0x7F3A1B2C"));
    CHECK(model.copy_text(1, slopkit::ui::models::CopyFormat::absolute) == QStringLiteral("0x7F3A1B2C"));
    CHECK(model.copy_text(1, slopkit::ui::models::CopyFormat::address_and_value) == QStringLiteral("0x7F3A1B2C: 100"));

    // Module + RVA ignores the display mode; address + value follows it.
    model.set_address_mode(slopkit::ui::AddressMode::absolute);
    CHECK(model.copy_text(0, slopkit::ui::models::CopyFormat::module_relative) == QStringLiteral("app+40"));
    CHECK(model.copy_text(0, slopkit::ui::models::CopyFormat::address_and_value) == QStringLiteral("0x1040: 10"));

    // An out-of-range row and a cleared model yield nothing.
    CHECK(model.copy_text(2, slopkit::ui::models::CopyFormat::absolute).isEmpty());
    CHECK(model.copy_text(-1, slopkit::ui::models::CopyFormat::module_relative).isEmpty());
    model.clear();
    CHECK(model.copy_text(0, slopkit::ui::models::CopyFormat::absolute).isEmpty());
}

TEST_CASE("the found-results model copies the value in hex when configured", "[ui]")
{
    application();

    slopkit::ui::models::FoundResultsModel model;

    slopkit::scan::ScanConfig config;
    config.value_type = slopkit::scan::ValueType::int32;
    config.hex        = true;

    slopkit::scan::ScanSnapshot snapshot;
    snapshot.hit_count = 1;
    snapshot.hits.push_back(slopkit::scan::ScanHit {
        0x1040, std::vector<std::byte> {std::byte {10}, std::byte {0}, std::byte {0}, std::byte {0}},
          {}
    });
    model.set_snapshot(snapshot, config);
    model.set_modules({module_image("app", 0x1000, 0x1000)});

    // The value matches the Value column's hex rendering, zero-padded to int32.
    CHECK(model.copy_text(0, slopkit::ui::models::CopyFormat::address_and_value)
          == QStringLiteral("app+40: 0x0000000A"));
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

    // Images deliberately out of order; the model sorts them.
    std::vector<slopkit::process::ModuleInfo> modules;
    modules.push_back(module_image("low", 0x2000, 0x1000));
    modules.push_back(module_image("first", 0x1000, 0x800));
    model.set_modules(modules);

    // Static hits group first in address order, the heap hit last.
    CHECK(model.hit_at(0)->address == 0x1000);
    CHECK(model.hit_at(1)->address == 0x2000);
    CHECK(model.hit_at(2)->address == 0x5000000);
    CHECK(model.is_static(0x1000));
    CHECK(model.is_static(0x17FF));
    CHECK_FALSE(model.is_static(0x1800)); // Half-open: base + size is outside.
    CHECK_FALSE(model.is_static(0x5000000));

    // Static hits render as module+RVA by default; the heap hit stays absolute.
    CHECK(model.data(model.index(0, slopkit::ui::models::FoundResultsModel::address), Qt::DisplayRole).toString()
          == QStringLiteral("first+0"));
    CHECK(model.data(model.index(1, slopkit::ui::models::FoundResultsModel::address), Qt::DisplayRole).toString()
          == QStringLiteral("low+0"));
    CHECK(model.data(model.index(2, slopkit::ui::models::FoundResultsModel::address), Qt::DisplayRole).toString()
          == QStringLiteral("0x5000000"));

    // Switching to absolute restores the raw address text everywhere.
    model.set_address_mode(slopkit::ui::AddressMode::absolute);
    CHECK(model.data(model.index(0, slopkit::ui::models::FoundResultsModel::address), Qt::DisplayRole).toString()
          == QStringLiteral("0x1000"));
    CHECK(model.data(model.index(2, slopkit::ui::models::FoundResultsModel::address), Qt::DisplayRole).toString()
          == QStringLiteral("0x5000000"));
    model.set_address_mode(slopkit::ui::AddressMode::module_relative);

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

    // The tier is the primary key: the main-image hit (the lowest-based image
    // when nothing is flagged) stays above the other static hit, which stays
    // above the dynamic one, whatever the sort order.
    model.sort(slopkit::ui::models::FoundResultsModel::address, Qt::DescendingOrder);
    CHECK(model.hit_at(0)->address == 0x1000);
    CHECK(model.hit_at(1)->address == 0x2000);
    CHECK(model.hit_at(2)->address == 0x5000000);

    // Dropping the map makes every hit non-static again.
    model.set_modules({});
    CHECK_FALSE(model.is_static(0x1000));
    CHECK_FALSE(model.is_static(0x2000));
    CHECK(model.rowCount() == 3);

    // A flagged main image outranks the other static image, which outranks the
    // dynamic hit, even though the flagged image has the higher base.
    model.sort(slopkit::ui::models::FoundResultsModel::address, Qt::AscendingOrder);
    slopkit::process::ModuleInfo game = module_image("game", 0x2000, 0x1000);
    game.is_main                      = true;
    model.set_modules({game, module_image("lib", 0x1000, 0x800)});

    CHECK(model.is_main_hit(0x2000));       // base included
    CHECK(model.is_main_hit(0x2FFF));       // base + size - 1 included
    CHECK_FALSE(model.is_main_hit(0x3000)); // half-open end excluded
    CHECK_FALSE(model.is_main_hit(0x1000));
    CHECK(model.hit_at(0)->address == 0x2000);    // main image
    CHECK(model.hit_at(1)->address == 0x1000);    // other static image
    CHECK(model.hit_at(2)->address == 0x5000000); // dynamic
}

namespace
{
    // A whole-list result set of 300 hits whose first 256 stored hits are all
    // heap hits, with one static hit at index 280, past the display page.
    std::shared_ptr<std::vector<slopkit::scan::ScanHit>> late_static_result()
    {
        auto whole = std::make_shared<std::vector<slopkit::scan::ScanHit>>();
        for (int i = 0; i < 300; ++i)
        {
            slopkit::scan::ScanHit hit;
            hit.address               = 0x100000 + static_cast<std::uint64_t>(i) * 4;
            // Big-endian so a byte-wise compare orders the values like the
            // integers they stand for.
            const std::uint32_t value = static_cast<std::uint32_t>(i);
            hit.value                 = {static_cast<std::byte>((value >> 24) & 0xFF),
                                         static_cast<std::byte>((value >> 16) & 0xFF),
                                         static_cast<std::byte>((value >> 8) & 0xFF),
                                         static_cast<std::byte>(value & 0xFF)};
            whole->push_back(std::move(hit));
        }
        (*whole)[280].address = 0x500000;
        return whole;
    }

    slopkit::scan::ScanSnapshot snapshot_over(const std::shared_ptr<const std::vector<slopkit::scan::ScanHit>>& whole)
    {
        slopkit::scan::ScanSnapshot snapshot;
        snapshot.hit_count = whole->size();
        snapshot.hits.assign(whole->begin(), whole->begin() + static_cast<std::ptrdiff_t>(slopkit::scan::kDisplayPage));
        snapshot.result_hits = whole;
        return snapshot;
    }
} // namespace

TEST_CASE("the found-results model orders the whole result set", "[ui]")
{
    application();

    slopkit::ui::models::FoundResultsModel model;

    slopkit::scan::ScanConfig config;
    config.value_type = slopkit::scan::ValueType::int32;

    const auto whole = late_static_result();
    model.set_snapshot(snapshot_over(whole), config);
    model.set_modules({module_image("late", 0x500000, 0x100)});

    // The page alone holds no static hit, yet the whole-list ordering puts the
    // late static hit first and still shows one page of rows.
    CHECK(model.rowCount() == slopkit::scan::kDisplayPage);
    CHECK(model.hit_at(0)->address == 0x500000);
    CHECK(model.is_static(model.hit_at(0)->address));
    CHECK_FALSE(model.is_static(model.hit_at(1)->address));
    // The rows after the static hit are the smallest addresses of the whole set.
    CHECK(model.hit_at(1)->address == 0x100000);
    CHECK(model.hit_at(255)->address == 0x100000 + 254 * 4);
}

TEST_CASE("the found-results model sorts over the whole result set", "[ui]")
{
    application();

    slopkit::ui::models::FoundResultsModel model;

    slopkit::scan::ScanConfig config;
    config.value_type = slopkit::scan::ValueType::int32;

    const auto whole = late_static_result();
    model.set_snapshot(snapshot_over(whole), config);
    model.set_modules({module_image("late", 0x500000, 0x100)});

    model.sort(slopkit::ui::models::FoundResultsModel::value, Qt::DescendingOrder);

    // Statics stay grouped first; the largest value of the whole set, which is
    // beyond the page, is shown right after it.
    CHECK(model.rowCount() == slopkit::scan::kDisplayPage);
    CHECK(model.hit_at(0)->address == 0x500000);
    CHECK(model.hit_at(0)->value == whole->at(280).value);
    CHECK(model.hit_at(1)->address == 0x100000 + 299 * 4);
    CHECK(model.hit_at(2)->address == 0x100000 + 298 * 4);
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

TEST_CASE("the found list stays empty until the scan finishes", "[ui]")
{
    application();

    // 300 matches at 4-byte spacing.
    auto bytes = std::make_shared<std::vector<std::byte>>(300 * 4, std::byte {0});
    for (std::size_t i = 0; i < 300; ++i)
    {
        const std::uint32_t value = 10;
        std::memcpy(bytes->data() + i * 4, &value, sizeof(value));
    }

    // Hold the scan's read so its running state stays observable.
    auto                        release = std::make_shared<std::atomic<bool>>(false);
    slopkit::scan::MemorySource source;
    source.read = [bytes,
                   release](std::uint64_t address,
                            std::size_t   size) -> std::expected<std::vector<std::byte>, slopkit::process::AccessError>
    {
        while (!release->load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (address < 0x1000 || address - 0x1000 + size > bytes->size())
        {
            return std::unexpected(slopkit::process::AccessError::not_found);
        }
        const auto offset = static_cast<std::size_t>(address - 0x1000);
        return std::vector<std::byte>(bytes->begin() + static_cast<std::ptrdiff_t>(offset),
                                      bytes->begin() + static_cast<std::ptrdiff_t>(offset + size));
    };
    source.regions = [bytes]()
    {
        slopkit::process::RegionInfo region;
        region.start    = 0x1000;
        region.end      = 0x1000 + bytes->size();
        region.readable = true;
        region.writable = true;
        return std::vector<slopkit::process::RegionInfo> {region};
    };

    slopkit::scan::ScanEngine engine;
    slopkit::scan::ScanConfig config;
    config.value = std::int64_t {10};
    engine.first_scan(config, source);
    REQUIRE(engine.is_running());

    slopkit::table::AddressTable        table;
    slopkit::ui::panels::FoundListPanel panel {engine, table};
    auto*                               view = panel.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    auto* model = dynamic_cast<slopkit::ui::models::FoundResultsModel*>(view->model());
    REQUIRE(model != nullptr);
    auto* header = panel.findChild<QLabel*>();
    REQUIRE(header != nullptr);

    // While the scan runs the list is empty and the header reports progress.
    panel.refresh();
    CHECK(model->rowCount() == 0);
    CHECK(header->text().startsWith(QStringLiteral("Scanning... ")));
    CHECK(view->selectionModel()->selectedRows().isEmpty());

    // Releasing the read lets the scan finish; the rows appear with it.
    release->store(true);
    for (int i = 0; i < 5000 && engine.is_running(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE_FALSE(engine.is_running());
    REQUIRE(engine.has_results());
    panel.refresh();
    CHECK(model->rowCount() == slopkit::scan::kDisplayPage);
    CHECK(header->text() == QStringLiteral("Showing 256 of 300 results"));
}

TEST_CASE("the found list keeps the rows when a refinement is cancelled", "[ui]")
{
    application();

    // 300 matches at 4-byte spacing.
    auto bytes = std::make_shared<std::vector<std::byte>>(300 * 4, std::byte {0});
    for (std::size_t i = 0; i < 300; ++i)
    {
        const std::uint32_t value = 10;
        std::memcpy(bytes->data() + i * 4, &value, sizeof(value));
    }

    // Hold the refinement's read so the cancel lands while it runs.
    auto                        block   = std::make_shared<std::atomic<bool>>(false);
    auto                        release = std::make_shared<std::atomic<bool>>(false);
    slopkit::scan::MemorySource source;
    source.read = [bytes, block, release](
                      std::uint64_t address,
                      std::size_t   size) -> std::expected<std::vector<std::byte>, slopkit::process::AccessError>
    {
        if (block->load())
        {
            while (!release->load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (address < 0x1000 || address - 0x1000 + size > bytes->size())
        {
            return std::unexpected(slopkit::process::AccessError::not_found);
        }
        const auto offset = static_cast<std::size_t>(address - 0x1000);
        return std::vector<std::byte>(bytes->begin() + static_cast<std::ptrdiff_t>(offset),
                                      bytes->begin() + static_cast<std::ptrdiff_t>(offset + size));
    };
    source.regions = [bytes]()
    {
        slopkit::process::RegionInfo region;
        region.start    = 0x1000;
        region.end      = 0x1000 + bytes->size();
        region.readable = true;
        region.writable = true;
        return std::vector<slopkit::process::RegionInfo> {region};
    };

    slopkit::scan::ScanEngine engine;
    slopkit::scan::ScanConfig config;
    config.value = std::int64_t {10};
    engine.first_scan(config, source);
    for (int i = 0; i < 5000 && engine.is_running(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(engine.has_results());

    slopkit::table::AddressTable        table;
    slopkit::ui::panels::FoundListPanel panel {engine, table};
    auto*                               view = panel.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    auto* model = dynamic_cast<slopkit::ui::models::FoundResultsModel*>(view->model());
    REQUIRE(model != nullptr);
    auto* header = panel.findChild<QLabel*>();
    REQUIRE(header != nullptr);

    panel.refresh();
    REQUIRE(model->rowCount() == slopkit::scan::kDisplayPage);
    REQUIRE(header->text() == QStringLiteral("Showing 256 of 300 results"));

    // Refine while its read is held, then cancel before it finishes.
    block->store(true);
    slopkit::scan::ScanConfig refinement;
    refinement.type       = slopkit::scan::ScanType::unchanged;
    refinement.value_type = slopkit::scan::ValueType::int32;
    engine.next_scan(refinement);
    REQUIRE(engine.is_running());

    panel.refresh();
    CHECK(model->rowCount() == 0);
    CHECK(header->text().startsWith(QStringLiteral("Scanning... ")));

    engine.cancel();
    release->store(true);
    for (int i = 0; i < 5000 && engine.is_running(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE_FALSE(engine.is_running());

    // The previous result set is back on screen.
    panel.refresh();
    CHECK(model->rowCount() == slopkit::scan::kDisplayPage);
    CHECK(header->text() == QStringLiteral("Showing 256 of 300 results"));
}

TEST_CASE("the found list row menu carries the copy submenu", "[ui]")
{
    application();

    slopkit::scan::ScanEngine           engine;
    slopkit::table::AddressTable        table;
    slopkit::ui::panels::FoundListPanel panel {engine, table};

    // An empty model still builds the row menu, so a stale row cannot break it.
    QMenu menu;
    panel.populate_row_menu(menu, 0);

    CHECK(action_texts(menu.actions())
          == QList<QString> {QStringLiteral("Add to address table"), QStringLiteral("Copy")});

    QMenu* copy = nullptr;
    for (QAction* action : menu.actions())
    {
        if (action->menu() != nullptr)
        {
            copy = action->menu();
        }
    }
    REQUIRE(copy != nullptr);
    CHECK(action_texts(copy->actions())
          == QList<QString> {QStringLiteral("Address (module + RVA)"),
                             QStringLiteral("Address (absolute)"),
                             QStringLiteral("Address + value")});

    // Triggering the entries with no hit behind them is a no-op, not a crash.
    menu.actions().front()->trigger();
    copy->actions().front()->trigger();
}

TEST_CASE("the found list shows the static-first top of the whole result set", "[ui]")
{
    application();

    // 300 matches at 4-byte spacing; the first page holds no static hit.
    std::vector<std::byte> bytes(300 * 4, std::byte {0});
    for (std::size_t i = 0; i < 300; ++i)
    {
        const std::uint32_t value = 10;
        std::memcpy(bytes.data() + i * 4, &value, sizeof(value));
    }

    slopkit::scan::ScanEngine engine;
    slopkit::scan::ScanConfig config;
    config.value = std::int64_t {10};
    engine.first_scan(config, slopkit::scan::make_buffer_source(bytes, 0x1000));
    for (int i = 0; i < 5000 && engine.is_running(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(engine.has_results());
    REQUIRE(engine.snapshot().hit_count == 300);

    slopkit::process::ModuleInfo image;
    image.base = 0x1400; // Covers the hits from index 256 upwards.
    image.size = 0x100;
    image.kind = slopkit::process::ModuleKind::elf;
    image.name = "app";
    image.path = "/opt/app";

    slopkit::table::AddressTable        table;
    slopkit::ui::panels::FoundListPanel panel {engine, table};
    panel.set_modules({image});
    panel.refresh();

    auto* view = panel.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    auto* model = dynamic_cast<slopkit::ui::models::FoundResultsModel*>(view->model());
    REQUIRE(model != nullptr);

    // The late static hit leads the one-page window, and the line reports the
    // rows on screen against every match of the scan.
    CHECK(model->rowCount() == slopkit::scan::kDisplayPage);
    REQUIRE(model->hit_at(0) != nullptr);
    CHECK(model->hit_at(0)->address == 0x1400);
    CHECK(model->is_static(model->hit_at(0)->address));

    auto* header = panel.findChild<QLabel*>();
    REQUIRE(header != nullptr);
    CHECK(header->text() == QStringLiteral("Showing 256 of 300 results"));

    // Double-clicking adds exactly the hit the row shows.
    const std::uint64_t first = model->hit_at(0)->address;
    REQUIRE(QMetaObject::invokeMethod(
        view,
        "doubleClicked",
        Qt::DirectConnection,
        Q_ARG(QModelIndex, model->index(0, slopkit::ui::models::FoundResultsModel::address))));
    REQUIRE(table.entries().size() == 1);
    CHECK(table.entries()[0].address == first);
}

TEST_CASE("the found list tooltips an elided cell", "[ui]")
{
    application();

    // One int32 hit whose hex rendering is wider than a collapsed column.
    std::vector<std::byte> bytes(4, std::byte {0});
    const std::uint32_t    value = 0x12345678;
    std::memcpy(bytes.data(), &value, sizeof(value));

    slopkit::scan::ScanEngine engine;
    slopkit::scan::ScanConfig config;
    config.value_type = slopkit::scan::ValueType::int32;
    config.value      = std::int64_t {0x12345678};
    config.hex        = true;
    engine.first_scan(config, slopkit::scan::make_buffer_source(bytes, 0x1000));
    for (int i = 0; i < 5000 && engine.is_running(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(engine.has_results());

    slopkit::table::AddressTable        table;
    slopkit::ui::panels::FoundListPanel panel {engine, table};
    panel.resize(400, 200);
    panel.show();
    panel.refresh();
    QCoreApplication::processEvents();

    auto* view = panel.findChild<QTableView*>();
    REQUIRE(view != nullptr);
    auto* model = dynamic_cast<slopkit::ui::models::FoundResultsModel*>(view->model());
    REQUIRE(model != nullptr);
    REQUIRE(model->rowCount() > 0);

    const QModelIndex cell = model->index(0, slopkit::ui::models::FoundResultsModel::value);
    const QString     text = model->data(cell, Qt::DisplayRole).toString();
    REQUIRE_FALSE(text.isEmpty());

    // A column wide enough for the value stays quiet: with no tooltip shown yet,
    // the hover must not create one.
    view->setColumnWidth(slopkit::ui::models::FoundResultsModel::value, 300);
    QCoreApplication::processEvents();
    const QPoint fitted_pos = view->visualRect(cell).center();
    QHelpEvent   fitted {QEvent::ToolTip, fitted_pos, view->viewport()->mapToGlobal(fitted_pos)};
    QApplication::sendEvent(view->viewport(), &fitted);
    CHECK_FALSE(QToolTip::isVisible());
    CHECK(QToolTip::text().isEmpty());

    // A column narrower than the value reveals the whole text on hover.
    view->setColumnWidth(slopkit::ui::models::FoundResultsModel::value, 1);
    QCoreApplication::processEvents();
    const QPoint clipped_pos = view->visualRect(cell).center();
    QHelpEvent   clipped {QEvent::ToolTip, clipped_pos, view->viewport()->mapToGlobal(clipped_pos)};
    QApplication::sendEvent(view->viewport(), &clipped);
    CHECK(QToolTip::isVisible());
    CHECK(QToolTip::text() == text);
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

TEST_CASE("the address-table model renders static addresses as module+RVA", "[ui]")
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

    const auto address_cell = [&model]()
    {
        return model.data(model.index(0, slopkit::ui::models::AddressTableModel::address), Qt::DisplayRole).toString();
    };

    // Without a module map the address stays absolute even in the default mode.
    CHECK(address_cell() == QStringLiteral("0x1040"));

    // Inside a module image the Address column renders name+RVA.
    model.set_modules({module_image("app", 0x1000, 0x1000)});
    CHECK(address_cell() == QStringLiteral("app+40"));

    // A heap address is never labelled.
    CHECK(model.address_text(0x5000000) == QStringLiteral("0x5000000"));

    // Absolute mode restores the raw text.
    model.set_address_mode(slopkit::ui::AddressMode::absolute);
    CHECK(address_cell() == QStringLiteral("0x1040"));
}

TEST_CASE("the address list delete confirmation follows the address mode", "[ui]")
{
    application();

    slopkit::table::AddressTable table;
    slopkit::table::AddressEntry entry;
    entry.address = 0x1040;
    entry.type    = slopkit::scan::ValueType::int32;
    entry.bytes   = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
    table.add(entry);
    table.set_selected(0);

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::panels::AddressListPanel panel {table, worker, target};

    // Dismisses the modal confirmation and returns its text. `delete_selected`
    // is synchronous, so the timer fires inside the box's nested event loop.
    const auto confirmation_text = [&panel]()
    {
        QString prompt;
        QTimer::singleShot(0,
                           [&prompt]
                           {
                               for (QWidget* widget : QApplication::topLevelWidgets())
                               {
                                   if (auto* box = qobject_cast<QMessageBox*>(widget);
                                       box != nullptr && box->isVisible())
                                   {
                                       prompt = box->text();
                                       box->reject();
                                       return;
                                   }
                               }
                           });
        panel.delete_selected();
        return prompt;
    };

    // No module map yet: the confirmation echoes the absolute address.
    CHECK(confirmation_text() == QStringLiteral("Delete 0x1040?"));

    // Inside a module image the confirmation echoes module+RVA.
    panel.set_modules({module_image("app", 0x1000, 0x1000)});
    CHECK(confirmation_text() == QStringLiteral("Delete app+40?"));

    // Absolute mode switches the confirmation back.
    panel.set_address_mode(slopkit::ui::AddressMode::absolute);
    CHECK(confirmation_text() == QStringLiteral("Delete 0x1040?"));
}

TEST_CASE("the main window shell is built", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    slopkit::ui::SettingsController  settings {scratch_settings_file("shell.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings};

    // The menu bar holds exactly File, View and Help; the Edit menu is gone.
    const QList<QAction*> menus = window.menuBar()->actions();
    REQUIRE(menus.size() == 3);
    CHECK(menus[0]->text() == QStringLiteral("File"));
    CHECK(menus[1]->text() == QStringLiteral("View"));
    CHECK(menus[2]->text() == QStringLiteral("Help"));

    CHECK(action_texts(menus[0]->menu()->actions())
          == QList<QString> {QStringLiteral("Open Process"),
                             QStringLiteral("Open Table"),
                             QStringLiteral("Save Table"),
                             QStringLiteral("Save Table As"),
                             QStringLiteral("Quit")});

    // The toolbar is gone; the file commands live only in the menus now.
    CHECK(window.findChild<QToolBar*>(QStringLiteral("main_toolbar")) == nullptr);

    // The View menu holds the log and settings entries; Help keeps About.
    CHECK(action_texts(menus[1]->menu()->actions())
          == QList<QString> {QStringLiteral("Log"), QStringLiteral("Settings")});
    CHECK(action_texts(menus[2]->menu()->actions()) == QList<QString> {QStringLiteral("About slopkit")});

    // No Edit menu survives, and Undo Scan / Add Address Manually... are gone as
    // menu actions anywhere in the window.
    for (QAction* menu : window.menuBar()->actions())
    {
        CHECK(menu->text() != QStringLiteral("Edit"));
    }
    for (QAction* action : window.findChildren<QAction*>())
    {
        CHECK(action->text() != QStringLiteral("Undo Scan"));
        CHECK(action->text() != QStringLiteral("Add Address Manually..."));
    }

    // Ctrl+T / Ctrl+O / Ctrl+S / Ctrl+Shift+S are bound to the file commands.
    const QList<QAction*> file_actions = menus[0]->menu()->actions();
    REQUIRE(file_actions.size() == 6);
    CHECK(file_actions[0]->shortcut() == QKeySequence(QStringLiteral("Ctrl+T")));
    CHECK(file_actions[1]->shortcut() == QKeySequence(QKeySequence::Open));
    CHECK(file_actions[2]->shortcut() == QKeySequence(QKeySequence::Save));
    CHECK(file_actions[3]->shortcut() == QKeySequence(QKeySequence::SaveAs));

    // The four file commands carry their action glyphs; every other menu entry
    // stays text-only.
    for (const int index : {0, 1, 2, 3})
    {
        CHECK_FALSE(file_actions[index]->icon().isNull());
        CHECK(file_actions[index]->icon().availableSizes().contains(QSize(16, 16)));
    }
    // Save and Save As share the same diskette glyph.
    CHECK(file_actions[2]->icon().pixmap(16).toImage() == file_actions[3]->icon().pixmap(16).toImage());
    // Quit, and every View/Help entry, remain icon-less.
    CHECK(file_actions[5]->icon().isNull());
    for (QAction* action : menus[1]->menu()->actions())
    {
        CHECK(action->icon().isNull());
    }
    for (QAction* action : menus[2]->menu()->actions())
    {
        CHECK(action->icon().isNull());
    }

    // View > Log opens the non-modal log window; live records reach its view
    // and the text filter hides what does not match.
    slopkit::log::Logger::instance().clear_history();
    slopkit::log::Logger::instance().set_minimum_level(slopkit::log::Level::info);

    QAction* log_action = menus[1]->menu()->actions().first();
    CHECK(log_action->text() == QStringLiteral("Log"));
    log_action->trigger();
    auto* log_dialog = window.findChild<slopkit::ui::dialogs::LogDialog*>();
    REQUIRE(log_dialog != nullptr);
    CHECK(log_dialog->isVisible());

    auto* log_view = log_dialog->findChild<QPlainTextEdit*>();
    REQUIRE(log_view != nullptr);
    slopkit::log::info("shell", "live record");
    QCoreApplication::processEvents();
    CHECK(log_view->toPlainText().contains(QStringLiteral("[shell] live record")));

    auto* log_search = log_dialog->findChild<QLineEdit*>();
    REQUIRE(log_search != nullptr);
    log_search->setText(QStringLiteral("no-such-record"));
    CHECK(log_view->toPlainText().isEmpty());
    log_search->clear();

    // Triggering the entry again raises the same window, not a second one.
    log_action->trigger();
    CHECK(window.findChildren<slopkit::ui::dialogs::LogDialog*>().size() == 1);

    slopkit::log::Logger::instance().set_minimum_level(slopkit::log::Level::info);

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

    // The removed idle hint is nowhere in the window, in any state.
    for (auto* label : window.findChildren<QLabel*>())
    {
        CHECK(label->text() != QStringLiteral("Select a process to enable scanning."));
    }

    // Show the window so the layouts are realised, then check that the hits
    // table's bottom edge is level with the Memory Scan Options panel's.
    window.show();
    QCoreApplication::processEvents();

    auto* hits = found_list->findChild<QTableView*>();
    REQUIRE(hits != nullptr);
    const auto hits_bottom    = hits->mapTo(&window, QPoint(0, hits->height()));
    const auto options_bottom = options_panel->mapTo(&window, QPoint(0, options_panel->height()));
    CHECK(hits_bottom.y() == options_bottom.y());

    // Growing the window hands every extra pixel to the address list: the scan
    // zone keeps its height and the two edges stay level.
    const int scan_zone_before = middle->height();
    const int address_before   = address_list->height();
    window.resize(window.width(), window.height() + 200);
    QCoreApplication::processEvents();
    CHECK(middle->height() == scan_zone_before);
    CHECK(address_list->height() - address_before >= 150);
    CHECK(hits->mapTo(&window, QPoint(0, hits->height())).y()
          == options_panel->mapTo(&window, QPoint(0, options_panel->height())).y());

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

TEST_CASE("the action icons are non-null multi-size icons", "[ui]")
{
    application();

    for (const auto which : {slopkit::ui::widgets::ActionIcon::open,
                             slopkit::ui::widgets::ActionIcon::save,
                             slopkit::ui::widgets::ActionIcon::target})
    {
        const QIcon icon = slopkit::ui::widgets::action_icon(which);
        CHECK_FALSE(icon.isNull());
        CHECK(icon.availableSizes().contains(QSize(16, 16)));
    }
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

TEST_CASE("the process list dialog keeps itself refreshed without controls", "[ui]")
{
    application();

    UiFakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::dialogs::ProcessListDialog dialog {worker, target};

    // The picker opens at its compact default size and locks it: it cannot be
    // resized, and it offers no minimize or maximize affordance.
    CHECK(dialog.size() == QSize(600, 440));
    CHECK(dialog.minimumSize() == dialog.maximumSize());
    CHECK_FALSE(dialog.windowFlags().testFlag(Qt::WindowMinimizeButtonHint));
    CHECK_FALSE(dialog.windowFlags().testFlag(Qt::WindowMaximizeButtonHint));

    dialog.resize(900, 700);
    CHECK(dialog.size() == dialog.minimumSize());

    // The picker blocks the main window while it is open.
    CHECK(dialog.windowModality() == Qt::ApplicationModal);
    CHECK(dialog.isModal());

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

    // Neither the Refresh button nor the Auto-refresh check box exists.
    CHECK(button_labelled(dialog, QStringLiteral("Refresh")) == nullptr);
    CHECK(checkbox_labelled(dialog, QStringLiteral("Auto-refresh")) == nullptr);
    CHECK(dialog.findChildren<QCheckBox*>().isEmpty());

    // The Plugins combo box leads the control row; the filter box takes the
    // remaining width.
    QComboBox* plugin_combo = nullptr;
    for (auto* combo : dialog.findChildren<QComboBox*>())
    {
        if (combo->count() > 0 && combo->itemText(0) == QStringLiteral("All plugins"))
        {
            plugin_combo = combo;
        }
    }
    REQUIRE(plugin_combo != nullptr);

    // The combo sizes itself to its entries instead of the empty box it had at
    // first show, so "All plugins" is never elided.
    CHECK(plugin_combo->sizeHint().width() > plugin_combo->fontMetrics().horizontalAdvance(plugin_combo->itemText(0)));

    QHBoxLayout* controls = nullptr;
    for (auto* row : dialog.findChildren<QHBoxLayout*>())
    {
        if (row->indexOf(plugin_combo) >= 0 && row->indexOf(search) >= 0)
        {
            controls = row;
        }
    }
    REQUIRE(controls != nullptr);
    CHECK(controls->indexOf(plugin_combo) < controls->indexOf(search));
    CHECK(controls->stretch(controls->indexOf(search)) == 1);

    // The control row is the dialog's first layout item: no status strip sits
    // above it.
    REQUIRE(dialog.layout() != nullptr);
    REQUIRE(dialog.layout()->count() > 0);
    CHECK(dialog.layout()->itemAt(0)->layout() == controls);

    // The old "N process(es) shown" counter is gone entirely.
    for (auto* label : dialog.findChildren<slopkit::ui::widgets::StatusLabel*>())
    {
        CHECK_FALSE(label->text().contains(QStringLiteral("process(es)")));
    }

    // Merely leaving the picker up re-lists the processes on its own.
    const int listed = access.list_calls.load();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return access.list_calls.load() > listed;
                    }));

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

    // The message is not tied to the detail pane: it survives losing the
    // selection.
    table->selectionModel()->clearCurrentIndex();
    QCoreApplication::processEvents();
    bool message_still_shown = false;
    for (auto* label : dialog.findChildren<slopkit::ui::widgets::StatusLabel*>())
    {
        if (label->text().contains(QStringLiteral("attach failed")))
        {
            message_still_shown = true;
        }
    }
    CHECK(message_still_shown);

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

    // Only the PID and Name columns survive.
    CHECK(model.columnCount() == 2);
    CHECK(model.headerData(slopkit::ui::dialogs::ProcessListModel::pid, Qt::Horizontal, Qt::DisplayRole).toString()
          == QStringLiteral("PID"));
    CHECK(model.headerData(slopkit::ui::dialogs::ProcessListModel::name, Qt::Horizontal, Qt::DisplayRole).toString()
          == QStringLiteral("Name"));

    // Sorting by Name still orders the visible rows.
    model.sort(slopkit::ui::dialogs::ProcessListModel::name, Qt::DescendingOrder);
    REQUIRE(model.process_at(0) != nullptr);
    CHECK(model.process_at(0)->pid == 200);

    // Restore the default PID ordering for the filtering checks below.
    model.sort(slopkit::ui::dialogs::ProcessListModel::pid, Qt::AscendingOrder);

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

TEST_CASE("a rejected scan is reported through the log, not a panel line", "[ui]")
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

    auto* value = address_field(panel, "Value");
    REQUIRE(value != nullptr);

    // Capture every record while the panel is exercised.
    std::vector<slopkit::log::Record> records;
    const auto                        sink = slopkit::log::Logger::instance().add_sink(
        [&records](const slopkit::log::Record& record)
        {
            records.push_back(record);
        });
    slopkit::log::Logger::instance().set_minimum_level(slopkit::log::Level::info);

    // A first scan needs a value to search for; the empty box is rejected.
    auto* scan_button = button_labelled(panel, QStringLiteral("First Scan"));
    REQUIRE(scan_button != nullptr);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        panel.refresh();
                        return scan_button->isEnabled();
                    }));
    value->clear();
    scan_button->click();

    slopkit::log::Logger::instance().remove_sink(sink);

    bool saw_reject = false;
    for (const auto& record : records)
    {
        if (record.category == "scan" && record.level == slopkit::log::Level::warning
            && record.message.starts_with("Value: "))
        {
            saw_reject = true;
        }
    }
    CHECK(saw_reject);

    // The removed idle hint is nowhere in the panel in any state.
    for (auto* label : panel.findChildren<QLabel*>())
    {
        CHECK(label->text() != QStringLiteral("Select a process to enable scanning."));
    }
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

TEST_CASE("the scan range dropdown pins the main image after All memory", "[ui]")
{
    application();

    UiFakeAccess access;

    slopkit::process::RegionInfo region;
    region.start    = 0x1000;
    region.end      = 0x9000;
    region.readable = true;
    access.regions  = {region};

    slopkit::process::ModuleInfo low;
    low.base = 0x1000;
    low.size = 0x800;
    low.kind = slopkit::process::ModuleKind::elf;
    low.name = "low";
    low.path = "/opt/low";

    slopkit::process::ModuleInfo game;
    game.base    = 0x5000;
    game.size    = 0x1000;
    game.kind    = slopkit::process::ModuleKind::elf;
    game.name    = "game";
    game.path    = "/opt/game";
    game.is_main = true;

    slopkit::process::ModuleInfo lib;
    lib.base = 0x8000;
    lib.size = 0x100;
    lib.kind = slopkit::process::ModuleKind::elf;
    lib.name = "lib";
    lib.path = "/opt/lib";

    slopkit::process::ModuleInfo anon;
    anon.base = 0x3000;
    anon.size = 0x1000;
    anon.kind = slopkit::process::ModuleKind::anonymous;
    anon.name = "[anon]";

    // Deliberately out of base order; the flagged image is not the lowest-based.
    access.modules = {low, anon, game, lib};

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

    // Item 0 is the whole process, then the flagged main image, then the
    // remaining file-backed modules by base; the anonymous mapping stays out.
    REQUIRE(combo->count() == 4);
    CHECK(combo->itemText(0) == QStringLiteral("All memory"));
    CHECK(combo->itemText(1) == QStringLiteral("game"));
    CHECK(combo->itemText(2) == QStringLiteral("low"));
    CHECK(combo->itemText(3) == QStringLiteral("lib"));
    CHECK(combo->itemData(1, Qt::ToolTipRole).toString() == QStringLiteral("/opt/game"));

    // Each row still narrows Start/Stop to that image's span.
    combo->setCurrentIndex(1);
    CHECK(start->text() == QStringLiteral("0x0000000000005000"));
    CHECK(stop->text() == QStringLiteral("0x0000000000006000"));

    combo->setCurrentIndex(2);
    CHECK(start->text() == QStringLiteral("0x0000000000001000"));
    CHECK(stop->text() == QStringLiteral("0x0000000000001800"));
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

    const auto address_for = [](std::uint64_t low_entry, bool flag_high = false)
    {
        UiFakeAccess access;

        slopkit::process::ModuleInfo high;
        high.base    = 0x2000;
        high.size    = 0x100;
        high.entry   = 0x2100;
        high.kind    = slopkit::process::ModuleKind::elf;
        high.name    = "high";
        high.path    = "/opt/high";
        high.is_main = flag_high;

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

    // A flagged image wins even when another file-backed module has a lower
    // base, and its entry point (not the base) is reported.
    CHECK(address_for(0x1040, true) == 0x2100);

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
    slopkit::ui::SettingsController  settings {scratch_settings_file("entry_row.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, settings};

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
    // The main module's map is loaded, so the box shows module+RVA by default.
    CHECK(address_edit->text() == QStringLiteral("low+40"));

    // The add button reaches the same non-modal dialog as the menu action.
    auto* add_dialog = window.findChild<slopkit::ui::dialogs::AddAddressDialog*>();
    REQUIRE(add_dialog != nullptr);
    CHECK_FALSE(add_dialog->isVisible());
    add_address->click();
    CHECK(add_dialog->isVisible());
}

TEST_CASE("the memory dump model renders static rows as module+RVA", "[ui]")
{
    application();

    slopkit::ui::dialogs::MemoryDumpModel model;
    model.set_page(0x1000, std::vector<std::byte>(32, std::byte {0}));

    const auto address_cell = [&model](int row)
    {
        return model.data(model.index(row, slopkit::ui::dialogs::MemoryDumpModel::address), Qt::DisplayRole).toString();
    };

    // Without a module map the column keeps the 16-digit padded text.
    CHECK(address_cell(1) == QStringLiteral("0x0000000000001010"));

    model.set_modules({module_image("app", 0x1000, 0x1000)});
    CHECK(address_cell(0) == QStringLiteral("app+0"));
    CHECK(address_cell(1) == QStringLiteral("app+10"));

    // Absolute mode restores the padded column.
    model.set_address_mode(slopkit::ui::AddressMode::absolute);
    CHECK(address_cell(1) == QStringLiteral("0x0000000000001010"));

    // A page outside every span stays absolute in both modes.
    model.set_address_mode(slopkit::ui::AddressMode::module_relative);
    model.set_page(0x5000000, std::vector<std::byte>(32, std::byte {0}));
    CHECK(address_cell(1) == QStringLiteral("0x0000000005000010"));
}

TEST_CASE("the memory viewer box accepts module-relative addresses", "[ui]")
{
    application();

    slopkit::plugin::PluginHost      host;
    slopkit::process::PluginAccess   access {host};
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();

    slopkit::ui::dialogs::MemoryViewerDialog viewer {worker, target};
    auto*                                    address_edit = viewer.findChild<QLineEdit*>();
    REQUIRE(address_edit != nullptr);
    auto* go = button_labelled(viewer, QStringLiteral("Go"));
    REQUIRE(go != nullptr);

    viewer.set_modules({module_image("app", 0x1000, 0x1000)});

    // set_address writes the display text into the box.
    viewer.set_address(0x1040);
    CHECK(address_edit->text() == QStringLiteral("app+40"));

    // An address outside every module keeps today's text.
    viewer.set_address(0x5000000);
    CHECK(address_edit->text() == QStringLiteral("0x5000000"));

    // Pasting a module+RVA (case-insensitively) and pressing Go navigates.
    viewer.set_address(0x1000);
    address_edit->setText(QStringLiteral("APP+40"));
    go->click();
    CHECK(address_edit->text() == QStringLiteral("app+40"));

    // An unparsable value leaves the page where it was: the box keeps the raw input.
    address_edit->setText(QStringLiteral("missing+40"));
    go->click();
    CHECK(address_edit->text() == QStringLiteral("missing+40"));
}

TEST_CASE("the settings dialog offers the address display choice", "[ui]")
{
    application();

    slopkit::plugin::PluginHost          host;
    slopkit::scan::ScanEngine            engine;
    slopkit::ui::SettingsController      controller {scratch_settings_file("settings_dialog.ini")};
    slopkit::ui::dialogs::SettingsDialog settings {host, engine, controller};

    auto* categories = settings.findChild<QListWidget*>();
    REQUIRE(categories != nullptr);
    REQUIRE(categories->count() == 5);
    CHECK(categories->item(0)->text() == QStringLiteral("Appearance"));
    CHECK(categories->item(1)->text() == QStringLiteral("Addresses"));
    CHECK(categories->item(2)->text() == QStringLiteral("Scanning"));

    auto* module_relative = radio_labelled(settings, QStringLiteral("Module + RVA"));
    REQUIRE(module_relative != nullptr);
    auto* absolute = radio_labelled(settings, QStringLiteral("Absolute address"));
    REQUIRE(absolute != nullptr);
    CHECK(module_relative->isChecked());
    CHECK_FALSE(absolute->isChecked());

    int                      changes = 0;
    slopkit::ui::AddressMode last    = slopkit::ui::AddressMode::module_relative;
    QObject::connect(&controller,
                     &slopkit::ui::SettingsController::addressModeChanged,
                     &controller,
                     [&](slopkit::ui::AddressMode mode)
                     {
                         ++changes;
                         last = mode;
                     });

    absolute->click();
    CHECK(changes == 1);
    CHECK(last == slopkit::ui::AddressMode::absolute);
    CHECK(absolute->isChecked());
    CHECK(controller.values().address_mode == slopkit::ui::AddressMode::absolute);

    // Re-selecting the same value is a no-op: no signal and no radio change.
    controller.set_address_mode(slopkit::ui::AddressMode::absolute);
    CHECK(changes == 1);

    // A real programmatic change keeps the radios in step.
    controller.set_address_mode(slopkit::ui::AddressMode::module_relative);
    CHECK(module_relative->isChecked());
    CHECK(changes == 2);

    // Help > About still reaches the About page after the extra category.
    settings.select_about();
    REQUIRE(categories->currentItem() != nullptr);
    CHECK(categories->currentRow() == categories->count() - 1);
    CHECK(categories->currentItem()->text() == QStringLiteral("About"));
}

TEST_CASE("the Addresses setting switches the viewer live", "[ui]")
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
    slopkit::ui::SettingsController  controller {scratch_settings_file("viewer_live.ini")};
    slopkit::ui::MainWindow          window {worker, target, host, controller};

    attach_app_session(worker);

    auto* scanner = window.findChild<slopkit::ui::panels::ScannerPanel*>();
    REQUIRE(scanner != nullptr);
    REQUIRE(pump_ui(worker,
                    [scanner]
                    {
                        return scanner->main_module_address() != 0;
                    }));

    auto* viewer = window.findChild<slopkit::ui::dialogs::MemoryViewerDialog*>();
    REQUIRE(viewer != nullptr);
    auto* address_edit = viewer->findChild<QLineEdit*>();
    REQUIRE(address_edit != nullptr);

    viewer->set_address(0x1040);
    CHECK(address_edit->text() == QStringLiteral("low+40"));

    auto* settings = window.findChild<slopkit::ui::dialogs::SettingsDialog*>();
    REQUIRE(settings != nullptr);
    auto* module_relative = radio_labelled(*settings, QStringLiteral("Module + RVA"));
    REQUIRE(module_relative != nullptr);
    auto* absolute = radio_labelled(*settings, QStringLiteral("Absolute address"));
    REQUIRE(absolute != nullptr);
    CHECK(module_relative->isChecked());

    // Clicking the setting re-renders the viewer immediately, with no reload.
    absolute->click();
    CHECK(address_edit->text() == QStringLiteral("0x1040"));

    module_relative->click();
    CHECK(address_edit->text() == QStringLiteral("low+40"));
}

TEST_CASE("the window applies the persisted settings at construction", "[ui]")
{
    application();

    const QString path = scratch_settings_file("persisted.ini");
    {
        std::ofstream file(path.toStdString(), std::ios::binary | std::ios::trunc);
        file << "[appearance]\ndark_theme=false\n[addresses]\ndisplay_mode=absolute\n";
    }

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
    slopkit::ui::SettingsController  settings {path};
    CHECK_FALSE(settings.values().dark_theme);
    CHECK(settings.values().address_mode == slopkit::ui::AddressMode::absolute);

    slopkit::ui::MainWindow window {worker, target, host, settings};

    attach_app_session(worker);

    auto* scanner = window.findChild<slopkit::ui::panels::ScannerPanel*>();
    REQUIRE(scanner != nullptr);
    REQUIRE(pump_ui(worker,
                    [scanner]
                    {
                        return scanner->main_module_address() != 0;
                    }));

    // The viewer opens in the persisted absolute mode even though the module map
    // resolved.
    auto* viewer = window.findChild<slopkit::ui::dialogs::MemoryViewerDialog*>();
    REQUIRE(viewer != nullptr);
    auto* address_edit = viewer->findChild<QLineEdit*>();
    REQUIRE(address_edit != nullptr);
    viewer->set_address(0x1040);
    CHECK(address_edit->text() == QStringLiteral("0x1040"));

    // The Settings dialog reflects both persisted values without user input.
    auto* dialog = window.findChild<slopkit::ui::dialogs::SettingsDialog*>();
    REQUIRE(dialog != nullptr);
    auto* light = radio_labelled(*dialog, QStringLiteral("Light"));
    REQUIRE(light != nullptr);
    CHECK(light->isChecked());
    auto* absolute = radio_labelled(*dialog, QStringLiteral("Absolute address"));
    REQUIRE(absolute != nullptr);
    CHECK(absolute->isChecked());
}

TEST_CASE("the log dialog shows live records and filters them", "[ui]")
{
    application();

    using slopkit::log::Level;

    slopkit::log::Logger::instance().clear_history();
    slopkit::log::Logger::instance().set_minimum_level(Level::debug);

    slopkit::ui::dialogs::LogDialog dialog;
    dialog.show();

    auto* view = dialog.findChild<QPlainTextEdit*>();
    REQUIRE(view != nullptr);
    auto* combo = dialog.findChild<QComboBox*>();
    REQUIRE(combo != nullptr);
    auto* search = dialog.findChild<QLineEdit*>();
    REQUIRE(search != nullptr);
    auto* status = dialog.findChild<slopkit::ui::widgets::StatusLabel*>(QStringLiteral("log_status"));
    REQUIRE(status != nullptr);

    // A record logged through the logger reaches the view once the queued
    // wake-up is processed, with its level and category visible.
    slopkit::log::warning("scan", "something happened");
    QCoreApplication::processEvents();
    CHECK(view->toPlainText().contains(QStringLiteral("warning")));
    CHECK(view->toPlainText().contains(QStringLiteral("[scan] something happened")));

    // The level filter hides records below the selected level.
    combo->setCurrentIndex(4); // Error
    CHECK_FALSE(view->toPlainText().contains(QStringLiteral("something happened")));
    combo->setCurrentIndex(3); // Warning
    CHECK(view->toPlainText().contains(QStringLiteral("something happened")));

    // The text filter is a case-insensitive substring over level, category and
    // message.
    search->setText(QStringLiteral("SCAN"));
    CHECK(view->toPlainText().contains(QStringLiteral("something happened")));
    search->setText(QStringLiteral("nope"));
    CHECK(view->toPlainText().isEmpty());

    // Clear empties the view and the retained history.
    search->clear();
    auto* clear = button_labelled(dialog, QStringLiteral("Clear"));
    REQUIRE(clear != nullptr);
    clear->click();
    CHECK(view->toPlainText().isEmpty());
    CHECK(slopkit::log::Logger::instance().history().empty());
    CHECK(status->text().contains(QStringLiteral("0 record(s)")));

    slopkit::log::Logger::instance().set_minimum_level(Level::info);
}

TEST_CASE("the log dialog seeds itself from the retained history", "[ui]")
{
    application();

    using slopkit::log::Level;

    slopkit::log::Logger::instance().clear_history();
    slopkit::log::Logger::instance().set_minimum_level(Level::info);

    slopkit::log::info("plugin", "logged before the window opened");

    slopkit::ui::dialogs::LogDialog dialog;
    auto*                           view = dialog.findChild<QPlainTextEdit*>();
    REQUIRE(view != nullptr);

    // Nothing is shown until the window is first opened.
    CHECK(view->toPlainText().isEmpty());

    // A record queued between construction and the first show must not be
    // duplicated by the history seeding.
    slopkit::log::info("ui", "queued before the first show");
    dialog.show();

    const QString text = view->toPlainText();
    CHECK(text.contains(QStringLiteral("[plugin] logged before the window opened")));
    CHECK(text.count(QStringLiteral("queued before the first show")) == 1);

    slopkit::log::Logger::instance().set_minimum_level(Level::info);
}
