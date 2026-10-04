#include <catch2/catch.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "process/types.hpp"
#include "scan/types.hpp"
#include "table/address_table.hpp"
#include "table/serializer.hpp"
#include "table/table_settings.hpp"

namespace
{
    using slopkit::process::ProcessInfo;
    using slopkit::scan::ValueType;
    using slopkit::table::AddressEntry;
    using slopkit::table::AddressTable;
    using slopkit::table::TableSettings;

    constexpr std::uint64_t kBase = 0x1000;

    AddressEntry make_entry(std::uint64_t address, ValueType type, std::initializer_list<int> bytes)
    {
        AddressEntry entry;
        entry.address = address;
        entry.type    = type;
        for (const int value : bytes)
        {
            entry.bytes.push_back(static_cast<std::byte>(value));
        }
        return entry;
    }

    std::filesystem::path scratch_file(std::string_view name)
    {
        const auto      directory = std::filesystem::path(SLOPKIT_TMP_DIR) / "table_settings_test";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        return directory / name;
    }

    std::vector<std::string> read_lines(const std::filesystem::path& path)
    {
        std::vector<std::string> lines;
        std::ifstream            file(path);
        std::string              line;
        while (std::getline(file, line))
        {
            lines.push_back(line);
        }
        return lines;
    }

    ProcessInfo make_process(std::uint32_t pid, std::string name, std::string exe_path)
    {
        ProcessInfo info;
        info.pid      = pid;
        info.name     = std::move(name);
        info.exe_path = std::move(exe_path);
        return info;
    }
} // namespace

TEST_CASE("table settings default to empty and detect configuration", "[table]")
{
    TableSettings settings;
    CHECK(settings.empty());
    CHECK(settings.target_process.empty());
    CHECK(settings.exe_path.empty());
    CHECK_FALSE(settings.auto_attach);
    CHECK_FALSE(settings.match_exe_path);

    settings.target_process = "game";
    CHECK_FALSE(settings.empty());

    settings.target_process.clear();
    settings.exe_path = "/usr/bin/game";
    CHECK_FALSE(settings.empty());

    settings.exe_path.clear();
    settings.auto_attach = true;
    CHECK_FALSE(settings.empty());

    settings.auto_attach    = false;
    settings.match_exe_path = true;
    CHECK_FALSE(settings.empty());
}

TEST_CASE("table settings survive a save/load round-trip", "[table]")
{
    AddressTable table;
    table.settings().target_process = "game \"one\"";
    table.settings().exe_path       = "/usr/bin/game \"one\"";
    table.settings().auto_attach    = true;
    table.settings().match_exe_path = true;
    table.add(make_entry(kBase, ValueType::int32, {1, 2, 3, 4}));

    const auto path = scratch_file("roundtrip.skt");
    std::filesystem::remove(path);

    REQUIRE(slopkit::table::save(path, table).has_value());

    AddressTable loaded;
    REQUIRE(slopkit::table::load(path, loaded).has_value());
    std::filesystem::remove(path);

    CHECK(loaded.settings().target_process == "game \"one\"");
    CHECK(loaded.settings().exe_path == "/usr/bin/game \"one\"");
    CHECK(loaded.settings().auto_attach);
    CHECK(loaded.settings().match_exe_path);
    CHECK(loaded.size() == 1);
}

TEST_CASE("a settings line without an executable path still loads", "[table]")
{
    const auto path = scratch_file("legacy_settings.skt");
    std::filesystem::remove(path);
    {
        std::ofstream file(path);
        file << "slopkit-table 1\n";
        file << "settings target=\"old\" auto_attach=1 match_exe_path=1\n";
    }

    AddressTable table;
    REQUIRE(slopkit::table::load(path, table).has_value());
    std::filesystem::remove(path);

    CHECK(table.settings().target_process == "old");
    CHECK(table.settings().exe_path.empty());
    CHECK(table.settings().auto_attach);
    CHECK(table.settings().match_exe_path);
}

TEST_CASE("a settings-only table round-trips its settings", "[table]")
{
    AddressTable table;
    table.settings().target_process = "solo";
    table.settings().auto_attach    = true;

    const auto path = scratch_file("settings_only.skt");
    std::filesystem::remove(path);

    REQUIRE(slopkit::table::save(path, table).has_value());

    AddressTable loaded;
    REQUIRE(slopkit::table::load(path, loaded).has_value());
    std::filesystem::remove(path);

    CHECK(loaded.size() == 0);
    CHECK(loaded.settings().target_process == "solo");
    CHECK(loaded.settings().auto_attach);
    CHECK_FALSE(loaded.settings().match_exe_path);
}

TEST_CASE("a legacy table without a settings line resets the settings", "[table]")
{
    const auto path = scratch_file("legacy.skt");
    std::filesystem::remove(path);
    {
        std::ofstream file(path);
        file << "slopkit-table 1\n";
        file << "entry description=\"x\" address=0x1000 type=i32 frozen=0 hex=0 value=01000000\n";
    }

    AddressTable table;
    table.settings().target_process = "stale";
    table.settings().auto_attach    = true;

    REQUIRE(slopkit::table::load(path, table).has_value());
    std::filesystem::remove(path);

    CHECK(table.settings().empty());
    CHECK(table.size() == 1);
}

TEST_CASE("the serializer rejects malformed settings lines", "[table]")
{
    const auto path = scratch_file("malformed.skt");

    SECTION("unknown key")
    {
        {
            std::ofstream file(path);
            file << "slopkit-table 1\n";
            file << "settings nonsense=1\n";
        }
        AddressTable table;
        const auto   result = slopkit::table::load(path, table);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("line 2") != std::string::npos);
    }

    SECTION("invalid boolean")
    {
        {
            std::ofstream file(path);
            file << "slopkit-table 1\n";
            file << "settings auto_attach=maybe\n";
        }
        AddressTable table;
        const auto   result = slopkit::table::load(path, table);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("line 2") != std::string::npos);
    }

    std::filesystem::remove(path);
}

TEST_CASE("an empty settings value is not written to the file", "[table]")
{
    AddressTable table;
    table.add(make_entry(kBase, ValueType::int32, {1, 0, 0, 0}));

    const auto path = scratch_file("no_settings.skt");
    std::filesystem::remove(path);

    REQUIRE(slopkit::table::save(path, table).has_value());
    const auto lines = read_lines(path);
    std::filesystem::remove(path);

    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "slopkit-table 1");
    CHECK(lines[1].starts_with("entry "));
}

TEST_CASE("a settings line after entry lines is still parsed", "[table]")
{
    const auto path = scratch_file("settings_after_entries.skt");
    std::filesystem::remove(path);
    {
        std::ofstream file(path);
        file << "slopkit-table 1\n";
        file << "entry description=\"x\" address=0x1000 type=i32 frozen=0 hex=0 value=01000000\n";
        file << "settings target=\"late\" auto_attach=1 match_exe_path=0\n";
    }

    AddressTable table;
    REQUIRE(slopkit::table::load(path, table).has_value());
    std::filesystem::remove(path);

    CHECK(table.settings().target_process == "late");
    CHECK(table.settings().auto_attach);
}

TEST_CASE("processes are matched by name, exe basename and lowest pid", "[process]")
{
    const ProcessInfo alpha     = make_process(20, "alpha", "/usr/bin/alpha");
    const ProcessInfo alpha_low = make_process(10, "Alpha", "/opt/alpha");
    const ProcessInfo other     = make_process(5, "beta", "/usr/bin/beta");

    const std::vector<ProcessInfo> processes {alpha, alpha_low, other};

    SECTION("name match is case-insensitive")
    {
        const auto* match = slopkit::process::match_process_by_name(processes, "ALPHA", false);
        REQUIRE(match != nullptr);
        CHECK(match->pid == 10);
    }

    SECTION("the lowest pid wins")
    {
        const auto* match = slopkit::process::match_process_by_name(processes, "alpha", false);
        REQUIRE(match != nullptr);
        CHECK(match->pid == 10);
    }

    SECTION("the exe basename only matches when requested")
    {
        CHECK(slopkit::process::match_process_by_name(processes, "alpha", false) != nullptr);
        CHECK(slopkit::process::match_process_by_name(processes, "ALPHA", true) != nullptr);

        const ProcessInfo              path_only = make_process(7, "unrelated", "/usr/bin/targetexe");
        const std::vector<ProcessInfo> one {path_only};
        CHECK(slopkit::process::match_process_by_name(one, "targetexe", false) == nullptr);
        const auto* match = slopkit::process::match_process_by_name(one, "targetexe", true);
        REQUIRE(match != nullptr);
        CHECK(match->pid == 7);
    }

    SECTION("no match and an empty name return nullptr")
    {
        CHECK(slopkit::process::match_process_by_name(processes, "gamma", true) == nullptr);
        CHECK(slopkit::process::match_process_by_name(processes, "", true) == nullptr);
    }
}

TEST_CASE("processes are matched by executable path", "[process]")
{
    const ProcessInfo alpha = make_process(20, "alpha", "/usr/bin/alpha");
    const ProcessInfo beta  = make_process(10, "beta", "/usr/bin/Alpha");
    const ProcessInfo other = make_process(5, "gamma", "/usr/bin/gamma");

    const std::vector<ProcessInfo> processes {alpha, beta, other};

    SECTION("the exe basename match is case-insensitive")
    {
        const auto* match = slopkit::process::match_process_by_exe_path(processes, "/opt/ALPHA");
        REQUIRE(match != nullptr);
        CHECK(match->pid == 10);
    }

    SECTION("the lowest pid wins")
    {
        const ProcessInfo              copy = make_process(3, "copy", "/other/alpha");
        const std::vector<ProcessInfo> more {alpha, copy};
        const auto*                    match = slopkit::process::match_process_by_exe_path(more, "/usr/bin/alpha");
        REQUIRE(match != nullptr);
        CHECK(match->pid == 3);
    }

    SECTION("an empty path returns nullptr")
    {
        CHECK(slopkit::process::match_process_by_exe_path(processes, "") == nullptr);
    }

    SECTION("no match returns nullptr")
    {
        CHECK(slopkit::process::match_process_by_exe_path(processes, "/usr/bin/missing") == nullptr);
    }
}
