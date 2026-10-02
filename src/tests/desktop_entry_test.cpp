#include <catch2/catch.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "platform/linux/desktop_entry.hpp"

namespace
{
    constexpr std::string_view kFixture = R"([Desktop Entry]
Type=Application
Name=Widget Editor
Exec=widget-editor %U
Icon=widget
NoDisplay=false

[Desktop Action new]
Name=New Window
Exec=widget-editor --new
)";

    void write_file(const std::filesystem::path& path, std::string_view content)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file << content;
    }
} // namespace

TEST_CASE("desktop entry parses the main group", "[desktop_entry]")
{
    const auto entry = slopkit::platform::parse_desktop_entry(kFixture);
    REQUIRE(entry.has_value());
    CHECK(entry->name == "Widget Editor");
    CHECK(entry->exec == "widget-editor %U");
    CHECK(entry->type == "Application");
    CHECK_FALSE(entry->no_display);
    CHECK_FALSE(entry->hidden);
}

TEST_CASE("desktop entry ignores groups other than Desktop Entry", "[desktop_entry]")
{
    CHECK_FALSE(slopkit::platform::parse_desktop_entry("[Desktop Action new]\nName=New\nExec=x\n").has_value());
    CHECK_FALSE(slopkit::platform::parse_desktop_entry("Key=Value\n").has_value());
}

TEST_CASE("desktop entry reports NoDisplay and Hidden", "[desktop_entry]")
{
    const auto entry =
        slopkit::platform::parse_desktop_entry("[Desktop Entry]\nName=x\nType=Application\nNoDisplay=true\n");
    REQUIRE(entry.has_value());
    CHECK(entry->no_display);

    const auto hidden =
        slopkit::platform::parse_desktop_entry("[Desktop Entry]\nName=x\nType=Application\nHidden=true\n");
    REQUIRE(hidden.has_value());
    CHECK(hidden->hidden);
}

TEST_CASE("desktop exec basename strips arguments and field codes", "[desktop_entry]")
{
    CHECK(slopkit::platform::desktop_exec_basename("widget-editor %U").value() == "widget-editor");
    CHECK(slopkit::platform::desktop_exec_basename("/usr/bin/widget-editor --new").value() == "widget-editor");
    CHECK(slopkit::platform::desktop_exec_basename("\"/opt/My App/app\" %f").value() == "app");
    CHECK_FALSE(slopkit::platform::desktop_exec_basename("   ").has_value());
}

TEST_CASE("desktop application classification matches the executable basename", "[desktop_entry]")
{
    const std::vector<std::string> executables {"browser", "widget-editor"};

    CHECK(slopkit::platform::is_desktop_application("/usr/bin/widget-editor", executables));
    CHECK(slopkit::platform::is_desktop_application("widget-editor", executables));
    CHECK_FALSE(slopkit::platform::is_desktop_application("/usr/bin/other", executables));
    CHECK_FALSE(slopkit::platform::is_desktop_application("", executables));
    CHECK_FALSE(slopkit::platform::is_desktop_application("/usr/bin/widget-editor", {}));
}

TEST_CASE("scanning an applications directory collects visible applications", "[desktop_entry]")
{
    const auto root = std::filesystem::temp_directory_path() / "slopkit_desktop_entry_test";
    std::filesystem::remove_all(root);

    write_file(root / "applications/widget.desktop", kFixture);
    write_file(root / "applications/hidden.desktop",
               "[Desktop Entry]\nType=Application\nName=Hidden\nExec=hidden-app\nNoDisplay=true\n");
    write_file(root / "applications/link.desktop", "[Desktop Entry]\nType=Link\nName=Link\nExec=link-app\n");
    write_file(root / "applications/nested/deep.desktop",
               "[Desktop Entry]\nType=Application\nName=Deep\nExec=/opt/deep-app %F\n");

    const std::vector<std::filesystem::path> dirs {root / "applications"};
    const auto                               exes = slopkit::platform::scan_desktop_executables(dirs);
    std::filesystem::remove_all(root);

    REQUIRE(exes.size() == 2);
    CHECK(exes[0] == "deep-app");
    CHECK(exes[1] == "widget-editor");
}
