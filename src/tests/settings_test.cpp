#include <catch2/catch.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <QObject>
#include <QString>

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
    }

    SettingsController reloaded(path);
    REQUIRE(reloaded.values().dark_theme == false);
    REQUIRE(reloaded.values().address_mode == AddressMode::absolute);
    REQUIRE(reloaded.values().auto_load_last_table == true);
    REQUIRE(reloaded.values().last_table_path == QStringLiteral("/tmp/roundtrip.skt"));
    REQUIRE(reloaded.values().last_directory == QStringLiteral("/tmp"));
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
}

TEST_CASE("Settings signals fire once per real change", "[settings]")
{
    SettingsController controller(scratch_file("signals.ini"));

    int theme_changes     = 0;
    int mode_changes      = 0;
    int auto_load_changes = 0;
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

    controller.set_dark_theme(true);
    REQUIRE(theme_changes == 2);
    controller.set_auto_load_last_table(false);
    REQUIRE(auto_load_changes == 2);
}
