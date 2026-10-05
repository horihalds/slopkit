#include <catch2/catch.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <QObject>
#include <QString>

#include "support/ui_helpers.hpp"
#include "ui/settings.hpp"

namespace
{
    using slopkit::ui::AddressMode;
    using slopkit::ui::SettingsController;

    QString scratch_file(std::string_view name)
    {
        const auto      directory = std::filesystem::path(SLOPKIT_TMP_DIR) / "settings_test";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        const auto path = directory / name;
        std::filesystem::remove(path, error);
        return QString::fromStdString(path.string());
    }

    void write_text(const QString& path, std::string_view text)
    {
        std::ofstream file(path.toStdString(), std::ios::binary | std::ios::trunc);
        file << text;
    }
} // namespace

TEST_CASE("A missing settings file falls back to the defaults", "[settings]")
{
    SettingsController controller(scratch_file("missing.ini"));

    REQUIRE(controller.values().dark_theme == true);
    REQUIRE(controller.values().address_mode == AddressMode::module_relative);
    REQUIRE(controller.values().auto_load_last_table == false);
    REQUIRE(controller.values().last_table_path.isEmpty());
    REQUIRE(controller.values().last_directory.isEmpty());
    REQUIRE(controller.values().live_update_enabled == true);
    REQUIRE(controller.values().live_update_interval_ms == 250);
}

TEST_CASE("Settings survive a round-trip through the INI file", "[settings]")
{
    const QString path = scratch_file("roundtrip.ini");
    {
        SettingsController controller(path);
        controller.set_dark_theme(false);
        controller.set_address_mode(AddressMode::absolute);
        controller.set_auto_load_last_table(true);
        controller.set_last_table_path(QStringLiteral("/tmp/roundtrip.skt"));
        controller.set_last_directory(QStringLiteral("/tmp"));
        controller.set_live_update_enabled(false);
        controller.set_live_update_interval_ms(1000);
    }

    SettingsController reloaded(path);
    REQUIRE(reloaded.values().dark_theme == false);
    REQUIRE(reloaded.values().address_mode == AddressMode::absolute);
    REQUIRE(reloaded.values().auto_load_last_table == true);
    REQUIRE(reloaded.values().last_table_path == QStringLiteral("/tmp/roundtrip.skt"));
    REQUIRE(reloaded.values().last_directory == QStringLiteral("/tmp"));
    REQUIRE(reloaded.values().live_update_enabled == false);
    REQUIRE(reloaded.values().live_update_interval_ms == 1000);
}

TEST_CASE("A hand-written INI file is loaded", "[settings]")
{
    const QString path = scratch_file("handwritten.ini");
    write_text(path,
               "[appearance]\ndark_theme=false\n[addresses]\ndisplay_mode=absolute\n"
               "[tables]\nauto_load_last=true\nlast_path=/tmp/handwritten.skt\n"
               "[files]\nlast_directory=/tmp/handwritten\n");

    SettingsController controller(path);
    REQUIRE(controller.values().dark_theme == false);
    REQUIRE(controller.values().address_mode == AddressMode::absolute);
    REQUIRE(controller.values().auto_load_last_table == true);
    REQUIRE(controller.values().last_table_path == QStringLiteral("/tmp/handwritten.skt"));
    REQUIRE(controller.values().last_directory == QStringLiteral("/tmp/handwritten"));
}

TEST_CASE("Unrecognised settings values fall back to the defaults", "[settings]")
{
    const QString path = scratch_file("junk.ini");
    write_text(path,
               "[appearance]\ndark_theme=not-a-bool\n[addresses]\ndisplay_mode=sideways\n"
               "[tables]\nauto_load_last=maybe\n");

    SettingsController controller(path);
    REQUIRE(controller.values().dark_theme == true);
    REQUIRE(controller.values().address_mode == AddressMode::module_relative);
    REQUIRE(controller.values().auto_load_last_table == false);
    REQUIRE(controller.values().live_update_enabled == true);
    REQUIRE(controller.values().live_update_interval_ms == 250);
}

TEST_CASE("The live-update interval is clamped into range", "[settings]")
{
    const QString low_path = scratch_file("live_low.ini");
    write_text(low_path, "liveUpdateIntervalMs=1\n");
    SettingsController low {low_path};
    REQUIRE(low.values().live_update_interval_ms == 50);

    const QString high_path = scratch_file("live_high.ini");
    write_text(high_path, "liveUpdateIntervalMs=99999\n");
    SettingsController high {high_path};
    REQUIRE(high.values().live_update_interval_ms == 5000);

    const QString malformed_path = scratch_file("live_malformed.ini");
    write_text(malformed_path, "liveUpdateIntervalMs=soon\n");
    SettingsController malformed {malformed_path};
    REQUIRE(malformed.values().live_update_interval_ms == 250);
}

TEST_CASE("The live-update values survive a manual INI", "[settings]")
{
    const QString path = scratch_file("live_manual.ini");
    write_text(path, "liveUpdateEnabled=false\nliveUpdateIntervalMs=1000\n[appearance]\ndark_theme=true\n");

    SettingsController controller {path};
    REQUIRE(controller.values().live_update_enabled == false);
    REQUIRE(controller.values().live_update_interval_ms == 1000);
}

TEST_CASE("Settings signals fire once per real change", "[settings]")
{
    SettingsController controller(scratch_file("signals.ini"));

    int theme_changes     = 0;
    int mode_changes      = 0;
    int auto_load_changes = 0;
    int live_changes      = 0;
    int interval_changes  = 0;
    QObject::connect(&controller,
                     &SettingsController::darkThemeChanged,
                     [&theme_changes](bool)
                     {
                         ++theme_changes;
                     });
    QObject::connect(&controller,
                     &SettingsController::addressModeChanged,
                     [&mode_changes](AddressMode)
                     {
                         ++mode_changes;
                     });
    QObject::connect(&controller,
                     &SettingsController::autoLoadLastTableChanged,
                     [&auto_load_changes](bool)
                     {
                         ++auto_load_changes;
                     });
    QObject::connect(&controller,
                     &SettingsController::liveUpdateChanged,
                     [&live_changes](bool)
                     {
                         ++live_changes;
                     });
    QObject::connect(&controller,
                     &SettingsController::liveUpdateIntervalChanged,
                     [&interval_changes](int)
                     {
                         ++interval_changes;
                     });

    controller.set_dark_theme(false);
    REQUIRE(theme_changes == 1);
    controller.set_dark_theme(false);
    REQUIRE(theme_changes == 1);

    controller.set_address_mode(AddressMode::absolute);
    REQUIRE(mode_changes == 1);
    controller.set_address_mode(AddressMode::absolute);
    REQUIRE(mode_changes == 1);

    controller.set_auto_load_last_table(true);
    REQUIRE(auto_load_changes == 1);
    controller.set_auto_load_last_table(true);
    REQUIRE(auto_load_changes == 1);

    controller.set_live_update_enabled(false);
    REQUIRE(live_changes == 1);
    controller.set_live_update_enabled(false);
    REQUIRE(live_changes == 1);

    // A setter clamps before comparing, so an out-of-range value still changes
    // once and lands in range.
    controller.set_live_update_interval_ms(1);
    REQUIRE(interval_changes == 1);
    REQUIRE(controller.values().live_update_interval_ms == 50);
    controller.set_live_update_interval_ms(50);
    REQUIRE(interval_changes == 1);

    controller.set_dark_theme(true);
    REQUIRE(theme_changes == 2);
    controller.set_auto_load_last_table(false);
    REQUIRE(auto_load_changes == 2);
    controller.set_live_update_enabled(true);
    REQUIRE(live_changes == 2);
}

TEST_CASE("the window applies the persisted settings at construction", "[ui]")
{
    application();

    const QString path = scratch_settings_file("persisted.ini");
    {
        std::ofstream file(path.toStdString(), std::ios::binary | std::ios::trunc);
        file << "[appearance]\ndark_theme=false\n[addresses]\ndisplay_mode=absolute\n";
    }

    FakeAccess access;

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
    auto* document = viewer->findChild<slopkit::ui::components::MemoryViewDocument*>();
    REQUIRE(document != nullptr);
    viewer->set_address(0x1040);
    CHECK(document->display_text(0x1040) == QStringLiteral("0x1040"));

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
