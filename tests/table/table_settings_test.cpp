#include <catch2/catch.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
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
#include "table/table_zip.hpp"

namespace
{
    using slopkit::process::ProcessInfo;
    using slopkit::scan::ValueType;
    using slopkit::table::AddressEntry;
    using slopkit::table::AddressTable;
    using slopkit::table::ArchiveMember;
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

    std::vector<std::string> member_names(const std::filesystem::path& path)
    {
        std::vector<std::string> names;
        const auto               archive = slopkit::table::read_archive(path);
        if (archive)
        {
            for (const ArchiveMember& member : *archive)
            {
                names.push_back(member.name);
            }
        }
        return names;
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

TEST_CASE("a settings member without an executable path still loads", "[table]")
{
    const auto path = scratch_file("partial_settings.skt");
    std::filesystem::remove(path);
    REQUIRE(slopkit::table::write_archive(
                path,
                std::vector<ArchiveMember> {
                    { .name = "version.txt",                               .text = "slopkit-table 4\n"},
                    {.name = "settings.txt", .text = "target=\"old\" auto_attach=1 match_exe_path=1\n"},
                    {   .name = "index.txt",                                                .text = ""},
    })
                .has_value());

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

TEST_CASE("an archive without a settings member resets the settings", "[table]")
{
    AddressTable source;
    source.add(make_entry(kBase, ValueType::int32, {1, 0, 0, 0}));

    const auto path = scratch_file("no_settings.skt");
    std::filesystem::remove(path);
    REQUIRE(slopkit::table::save(path, source).has_value());

    AddressTable table;
    table.settings().target_process = "stale";
    table.settings().auto_attach    = true;

    REQUIRE(slopkit::table::load(path, table).has_value());
    std::filesystem::remove(path);

    CHECK(table.settings().empty());
    CHECK(table.size() == 1);
}

TEST_CASE("the serializer rejects malformed settings members", "[table]")
{
    const auto path = scratch_file("malformed_settings.skt");

    SECTION("unknown key")
    {
        std::filesystem::remove(path);
        REQUIRE(slopkit::table::write_archive(path,
                                              std::vector<ArchiveMember> {
                                                  { .name = "version.txt", .text = "slopkit-table 4\n"},
                                                  {.name = "settings.txt",      .text = "nonsense=1\n"},
                                                  {   .name = "index.txt",                  .text = ""},
        })
                    .has_value());
        AddressTable table;
        const auto   result = slopkit::table::load(path, table);
        std::filesystem::remove(path);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("settings.txt") != std::string::npos);
    }

    SECTION("invalid boolean")
    {
        std::filesystem::remove(path);
        REQUIRE(slopkit::table::write_archive(path,
                                              std::vector<ArchiveMember> {
                                                  { .name = "version.txt",   .text = "slopkit-table 4\n"},
                                                  {.name = "settings.txt", .text = "auto_attach=maybe\n"},
                                                  {   .name = "index.txt",                    .text = ""},
        })
                    .has_value());
        AddressTable table;
        const auto   result = slopkit::table::load(path, table);
        std::filesystem::remove(path);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("settings.txt") != std::string::npos);
    }
}

TEST_CASE("an empty settings value produces no settings member", "[table]")
{
    AddressTable table;
    table.add(make_entry(kBase, ValueType::int32, {1, 0, 0, 0}));

    const auto path = scratch_file("empty_settings.skt");
    std::filesystem::remove(path);

    REQUIRE(slopkit::table::save(path, table).has_value());
    const auto names = member_names(path);
    std::filesystem::remove(path);

    CHECK(std::find(names.begin(), names.end(), "settings.txt") == names.end());
    CHECK(std::find(names.begin(), names.end(), "index.txt") != names.end());
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
