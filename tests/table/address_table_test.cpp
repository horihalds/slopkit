#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "process/access_worker.hpp"
#include "scan/types.hpp"
#include "table/address_table.hpp"
#include "table/serializer.hpp"
#include "table/table_zip.hpp"

namespace
{
    using slopkit::process::AccessError;
    using slopkit::process::WriteItem;
    using slopkit::scan::ValueType;
    using slopkit::table::AddressEntry;
    using slopkit::table::AddressTable;
    using slopkit::table::ArchiveMember;
    using slopkit::table::EntryKind;
    using slopkit::table::read_archive;
    using slopkit::table::write_archive;

    // The base address the fixture entries use; no target is required since the
    // model only encodes and caches.
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
        const auto      directory = std::filesystem::path(SLOPKIT_TMP_DIR) / "address_table_test";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        return directory / name;
    }

    std::optional<std::string> member_text(const std::filesystem::path& path, std::string_view name)
    {
        const auto archive = read_archive(path);
        if (!archive)
        {
            return std::nullopt;
        }
        for (const ArchiveMember& member : *archive)
        {
            if (member.name == name)
            {
                return member.text;
            }
        }
        return std::nullopt;
    }

    std::vector<std::string> member_names(const std::filesystem::path& path)
    {
        std::vector<std::string> names;
        const auto               archive = read_archive(path);
        if (archive)
        {
            for (const ArchiveMember& member : *archive)
            {
                names.push_back(member.name);
            }
        }
        return names;
    }

    // Restores the process-wide log level on scope exit.
    class LevelGuard
    {
    public:
        LevelGuard() : previous_(slopkit::log::Logger::instance().minimum_level()) {}

        LevelGuard(const LevelGuard&)            = delete;
        LevelGuard& operator=(const LevelGuard&) = delete;

        ~LevelGuard()
        {
            slopkit::log::Logger::instance().set_minimum_level(previous_);
        }

    private:
        slopkit::log::Level previous_;
    };

    // Registers a sink for the lifetime of the guard.
    class SinkGuard
    {
    public:
        explicit SinkGuard(slopkit::log::Sink sink) : id_(slopkit::log::Logger::instance().add_sink(std::move(sink))) {}

        SinkGuard(const SinkGuard&)            = delete;
        SinkGuard& operator=(const SinkGuard&) = delete;

        ~SinkGuard()
        {
            slopkit::log::Logger::instance().remove_sink(id_);
        }

    private:
        slopkit::log::SinkId id_;
    };
} // namespace

TEST_CASE("address entries are added, selected and removed", "[table]")
{
    AddressTable table;
    CHECK(table.empty());

    AddressEntry first;
    first.description = "first";
    table.add(first);
    table.add(first);
    REQUIRE(table.size() == 2);
    CHECK(table.selected() == 1);

    // Every entry gets a non-zero, unique id assigned by the table.
    CHECK(table.entries()[0].id != 0);
    CHECK(table.entries()[1].id != 0);
    CHECK(table.entries()[0].id != table.entries()[1].id);

    table.set_selected(0);
    table.remove(0);
    REQUIRE(table.size() == 1);
    CHECK(table.selected() == 0);

    table.remove(0);
    CHECK(table.empty());
    CHECK(table.selected() == -1);

    table.remove(5); // out of range is a no-op
    CHECK(table.empty());
}

TEST_CASE("encode_value encodes the parsed bytes", "[table]")
{
    SECTION("encodes the parsed bytes")
    {
        AddressTable table;
        table.add(make_entry(kBase, ValueType::int32, {}));

        const auto encoded = table.encode_value(0, "1234");
        REQUIRE(encoded.has_value());
        REQUIRE(encoded->size() == 4);
        table.apply_write(table.entries()[0].id, *encoded);
        CHECK(table.display_value(0) == "1234");

        table.entries()[0].hex = true;
        const auto hexed       = table.encode_value(0, "0x2A");
        REQUIRE(hexed.has_value());
        table.apply_write(table.entries()[0].id, *hexed);
        CHECK(table.display_value(0) == "0000002A");
    }

    SECTION("rejects malformed input and unknown indices")
    {
        AddressTable table;
        table.add(make_entry(kBase, ValueType::int32, {}));

        CHECK_FALSE(table.encode_value(0, "not a number").has_value());
        CHECK_FALSE(table.encode_value(9, "1").has_value());
    }
}

TEST_CASE("apply_write updates only the matching entry", "[table]")
{
    AddressTable table;
    table.add(make_entry(kBase, ValueType::int32, {}));
    const std::uint64_t id = table.entries()[0].id;
    REQUIRE(id != 0);

    // An unknown id, e.g. a write for a removed entry, is ignored.
    table.apply_write(id + 12345, std::vector<std::byte> {std::byte {0x01}});
    CHECK(table.entries()[0].bytes.empty());

    table.apply_write(id, std::vector<std::byte> {std::byte {0x2A}, std::byte {0}, std::byte {0}, std::byte {0}});
    CHECK(table.display_value(0) == "42");
}

TEST_CASE("freeze_items returns only the active entries and honours the interval", "[table]")
{
    AddressTable table;

    auto active   = make_entry(kBase, ValueType::int32, {42, 0, 0, 0});
    active.active = true;
    table.add(active);

    auto inactive   = make_entry(kBase + 4, ValueType::int32, {55, 0, 0, 0});
    inactive.active = false;
    table.add(inactive);

    const std::vector<WriteItem> first = table.freeze_items(0.0, 0.1);
    REQUIRE(first.size() == 1);
    CHECK(first[0].id == table.entries()[0].id);
    CHECK(first[0].address == kBase);
    CHECK(first[0].bytes.size() == 4);

    // Inside the interval nothing is returned.
    CHECK(table.freeze_items(0.05, 0.1).empty());

    // Once the interval elapsed the active entry is returned again.
    CHECK(table.freeze_items(0.2, 0.1).size() == 1);
}

TEST_CASE("an address table round-trips through the archive", "[table]")
{
    AddressTable original;
    auto         health = make_entry(0x1234, ValueType::int32, {0x40, 0x00, 0x00, 0x00});
    health.description  = "health";
    health.active       = true;
    health.expression   = "1234";
    original.add(health);

    auto armor        = make_entry(0x7FFD1234, ValueType::float64, {0, 0, 0, 0, 0, 0, 0x10, 0x40});
    armor.description = "armor";
    armor.hex         = true;
    armor.expression  = "7FFD1234";
    original.add(armor);

    const auto path = scratch_file("roundtrip.skt");
    std::filesystem::remove(path);
    REQUIRE(slopkit::table::save(path, original).has_value());

    AddressTable loaded;
    REQUIRE(slopkit::table::load(path, loaded).has_value());
    std::filesystem::remove(path);

    REQUIRE(loaded.size() == 2);

    CHECK(loaded.entries()[0].description == "health");
    CHECK(loaded.entries()[0].type == ValueType::int32);
    CHECK_FALSE(loaded.entries()[0].hex);
    CHECK(loaded.entries()[0].expression == "1234");
    CHECK(loaded.entries()[0].address == 0x1234);
    CHECK(loaded.entries()[0].bytes.size() == 4);
    // The frozen flag and the cached bytes are session-only.
    CHECK_FALSE(loaded.entries()[0].active);
    CHECK(loaded.entries()[0].bytes == std::vector<std::byte>(4, std::byte {0}));

    CHECK(loaded.entries()[1].description == "armor");
    CHECK(loaded.entries()[1].type == ValueType::float64);
    CHECK(loaded.entries()[1].hex);
    CHECK(loaded.entries()[1].expression == "7FFD1234");
    CHECK(loaded.entries()[1].address == 0x7FFD1234);
    CHECK(loaded.entries()[1].bytes.size() == 8);

    // Ids are not persisted; the loader regenerates non-zero, unique ids.
    CHECK(loaded.entries()[0].id != 0);
    CHECK(loaded.entries()[1].id != 0);
    CHECK(loaded.entries()[0].id != loaded.entries()[1].id);
}

TEST_CASE("the archive holds version, index and one member per entry", "[table]")
{
    AddressTable table;
    auto         health = make_entry(0x1234, ValueType::int32, {1, 0, 0, 0});
    health.description  = "health";
    health.expression   = "1234";
    table.add(health);

    auto armor        = make_entry(0x2008, ValueType::int32, {2, 0, 0, 0});
    armor.description = "armor";
    armor.expression  = "0x2000+8";
    table.add(armor);

    const auto path = scratch_file("layout.skt");
    std::filesystem::remove(path);
    REQUIRE(slopkit::table::save(path, table).has_value());

    CHECK(member_names(path)
          == std::vector<std::string> {"version.txt", "entries/health.txt", "entries/armor.txt", "index.txt"});
    CHECK(member_text(path, "version.txt") == std::optional<std::string> {"slopkit-table 4\n"});
    CHECK(member_text(path, "index.txt") == std::optional<std::string> {"entries/health.txt\nentries/armor.txt\n"});
    CHECK(member_text(path, "entries/health.txt")
          == std::optional<std::string> {"type=i32 hex=0 size=4 expr=\"1234\"\n"});

    std::filesystem::remove(path);
}

TEST_CASE("an entry without an expression is stored as its hex address", "[table]")
{
    AddressTable table;
    table.add(make_entry(0x2A2B, ValueType::int32, {0, 0, 0, 0}));

    const auto path = scratch_file("no_expression.skt");
    std::filesystem::remove(path);
    REQUIRE(slopkit::table::save(path, table).has_value());

    CHECK(member_text(path, "entries/unnamed.txt")
          == std::optional<std::string> {"type=i32 hex=0 size=4 expr=\"2A2B\"\n"});

    AddressTable loaded;
    REQUIRE(slopkit::table::load(path, loaded).has_value());
    std::filesystem::remove(path);

    REQUIRE(loaded.size() == 1);
    CHECK(loaded.entries()[0].description == "unnamed");
    CHECK(loaded.entries()[0].expression == "2A2B");
    CHECK(loaded.entries()[0].address == 0x2A2B);
}

TEST_CASE("duplicate descriptions get numbered member names", "[table]")
{
    AddressTable table;
    auto         first = make_entry(0x1000, ValueType::int32, {0, 0, 0, 0});
    first.expression   = "1000";
    table.add(first);
    auto second       = make_entry(0x2000, ValueType::int32, {0, 0, 0, 0});
    second.expression = "2000";
    table.add(second);

    const auto path = scratch_file("duplicates.skt");
    std::filesystem::remove(path);
    REQUIRE(slopkit::table::save(path, table).has_value());

    CHECK(member_text(path, "index.txt") == std::optional<std::string> {"entries/unnamed.txt\nentries/unnamed2.txt\n"});
    CHECK(member_text(path, "entries/unnamed2.txt")
          == std::optional<std::string> {"type=i32 hex=0 size=4 expr=\"2000\"\n"});

    AddressTable loaded;
    REQUIRE(slopkit::table::load(path, loaded).has_value());
    std::filesystem::remove(path);

    REQUIRE(loaded.size() == 2);
    // The description is the member stem, so the collision-numbered second
    // member reloads as "unnamed2" rather than the empty description it began
    // with; another save is stable.
    CHECK(loaded.entries()[0].description == "unnamed");
    CHECK(loaded.entries()[1].description == "unnamed2");
}

TEST_CASE("a custom width survives the round-trip", "[table]")
{
    AddressTable table;
    auto         buffer = make_entry(0x3000, ValueType::string, {});
    buffer.description  = "buffer";
    buffer.expression   = "3000";
    buffer.bytes.assign(64, std::byte {0xAB});
    table.add(buffer);

    const auto path = scratch_file("width.skt");
    std::filesystem::remove(path);
    REQUIRE(slopkit::table::save(path, table).has_value());

    CHECK(member_text(path, "entries/buffer.txt")
          == std::optional<std::string> {"type=str hex=0 size=64 expr=\"3000\"\n"});

    AddressTable loaded;
    REQUIRE(slopkit::table::load(path, loaded).has_value());
    std::filesystem::remove(path);

    REQUIRE(loaded.size() == 1);
    CHECK(loaded.entries()[0].type == ValueType::string);
    CHECK(loaded.entries()[0].bytes.size() == 64);
}

TEST_CASE("an expression with quotes and spaces round-trips", "[table]")
{
    AddressTable table;
    auto         entry = make_entry(0x2000, ValueType::int32, {0x01, 0x00, 0x00, 0x00});
    entry.description  = "quoted";
    entry.expression   = "my \"module\"+0x10";
    table.add(entry);

    const auto path = scratch_file("expression.skt");
    std::filesystem::remove(path);
    REQUIRE(slopkit::table::save(path, table).has_value());

    AddressTable loaded;
    REQUIRE(slopkit::table::load(path, loaded).has_value());
    std::filesystem::remove(path);

    REQUIRE(loaded.size() == 1);
    CHECK(loaded.entries()[0].expression == "my \"module\"+0x10");
    // A module base cannot be resolved without a target, so the address stays 0
    // for the panel's resolve pass.
    CHECK(loaded.entries()[0].address == 0);
}

TEST_CASE("a pointer expression is left for the target resolve pass", "[table]")
{
    AddressTable table;
    auto         entry = make_entry(0x0, ValueType::int32, {0, 0, 0, 0});
    entry.description  = "pointer";
    entry.expression   = "app+0+8";
    table.add(entry);

    const auto path = scratch_file("pointer.skt");
    std::filesystem::remove(path);
    REQUIRE(slopkit::table::save(path, table).has_value());

    AddressTable loaded;
    REQUIRE(slopkit::table::load(path, loaded).has_value());
    std::filesystem::remove(path);

    REQUIRE(loaded.size() == 1);
    CHECK(loaded.entries()[0].address == 0);
    CHECK(loaded.entries()[0].expression == "app+0+8");
}

TEST_CASE("a damaged archive is rejected with the member name", "[table]")
{
    const auto version = ArchiveMember {.name = "version.txt", .text = "slopkit-table 4\n"};
    const auto body    = ArchiveMember {.name = "entries/health.txt", .text = "type=i32 hex=0 size=4 expr=\"1234\"\n"};

    SECTION("an unknown entry key")
    {
        const auto path = scratch_file("unknown_key.skt");
        std::filesystem::remove(path);
        REQUIRE(
            write_archive(path,
                          std::vector<ArchiveMember> {
                              version,
                              {.name = "entries/health.txt", .text = "type=i32 hex=0 size=4 frozen=1 expr=\"1234\"\n"},
                              {         .name = "index.txt",                           .text = "entries/health.txt\n"}
        })
                .has_value());

        AddressTable table;
        const auto   result = slopkit::table::load(path, table);
        std::filesystem::remove(path);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("entries/health.txt") != std::string::npos);
        CHECK(result.error().find("frozen") != std::string::npos);
    }

    SECTION("a dangling index line")
    {
        const auto path = scratch_file("dangling.skt");
        std::filesystem::remove(path);
        REQUIRE(
            write_archive(path,
                          std::vector<ArchiveMember> {
                              version, body, {.name = "index.txt", .text = "entries/health.txt\nentries/armor.txt\n"}
        })
                .has_value());

        AddressTable table;
        const auto   result = slopkit::table::load(path, table);
        std::filesystem::remove(path);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("index.txt") != std::string::npos);
        CHECK(result.error().find("entries/armor.txt") != std::string::npos);
    }

    SECTION("an unlisted entries member")
    {
        const auto path = scratch_file("unlisted.skt");
        std::filesystem::remove(path);
        REQUIRE(write_archive(path,
                              std::vector<ArchiveMember> {
                                  version,
                                  body,
                                  {.name = "entries/orphan.txt", .text = "type=i32 hex=0 size=4 expr=\"1\"\n"},
                                  {         .name = "index.txt",               .text = "entries/health.txt\n"}
        })
                    .has_value());

        AddressTable table;
        const auto   result = slopkit::table::load(path, table);
        std::filesystem::remove(path);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("entries/orphan.txt") != std::string::npos);
    }

    SECTION("an unknown entry extension")
    {
        const auto path = scratch_file("unknown_extension.skt");
        std::filesystem::remove(path);
        REQUIRE(write_archive(path,
                              std::vector<ArchiveMember> {
                                  version,
                                  {.name = "entries/notes.md",            .text = "hello\n"},
                                  {       .name = "index.txt", .text = "entries/notes.md\n"}
        })
                    .has_value());

        AddressTable table;
        const auto   result = slopkit::table::load(path, table);
        std::filesystem::remove(path);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("entries/notes.md") != std::string::npos);
        CHECK(result.error().find("unknown entry extension") != std::string::npos);
    }

    SECTION("an unknown format token")
    {
        const auto path = scratch_file("unknown_version.skt");
        std::filesystem::remove(path);
        REQUIRE(
            write_archive(path,
                          std::vector<ArchiveMember> {
                              {.name = "version.txt", .text = "slopkit-table 9\n"},
                              {  .name = "index.txt",                  .text = ""}
        })
                .has_value());

        AddressTable table;
        const auto   result = slopkit::table::load(path, table);
        std::filesystem::remove(path);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("version.txt") != std::string::npos);
    }

    SECTION("a missing version member")
    {
        const auto path = scratch_file("no_version.skt");
        std::filesystem::remove(path);
        REQUIRE(write_archive(path,
                              std::vector<ArchiveMember> {
                                  {.name = "index.txt", .text = ""}
        })
                    .has_value());

        AddressTable table;
        const auto   result = slopkit::table::load(path, table);
        std::filesystem::remove(path);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("version.txt") != std::string::npos);
    }

    SECTION("an unparsable expression")
    {
        const auto path = scratch_file("bad_expr.skt");
        std::filesystem::remove(path);
        REQUIRE(write_archive(path,
                              std::vector<ArchiveMember> {
                                  version,
                                  {.name = "entries/health.txt", .text = "type=i32 hex=0 size=4 expr=\"1+\"\n"},
                                  {         .name = "index.txt",                .text = "entries/health.txt\n"}
        })
                    .has_value());

        AddressTable table;
        const auto   result = slopkit::table::load(path, table);
        std::filesystem::remove(path);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("entries/health.txt") != std::string::npos);
    }

    SECTION("a file that is not an archive")
    {
        const auto path = scratch_file("plain.skt");
        std::filesystem::remove(path);
        {
            std::ofstream file(path);
            file << "slopkit-table 3\n";
        }

        AddressTable table;
        const auto   result = slopkit::table::load(path, table);
        std::filesystem::remove(path);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("not a slopkit table archive") != std::string::npos);
    }

    SECTION("a missing path")
    {
        AddressTable table;
        const auto   result = slopkit::table::load(scratch_file("missing.skt"), table);
        REQUIRE_FALSE(result.has_value());
    }
}

TEST_CASE("address entries can be moved with stable ids", "[table]")
{
    AddressTable table;
    const auto   add = [&table](const char* description, std::uint64_t address)
    {
        auto entry        = make_entry(address, ValueType::int32, {1, 0, 0, 0});
        entry.description = description;
        table.add(entry);
    };
    add("first", 0x1000);
    add("second", 0x2000);
    add("third", 0x3000);

    const std::vector<std::uint64_t> ids {table.entries()[0].id, table.entries()[1].id, table.entries()[2].id};

    table.set_selected(0);
    table.move(0, 2);
    CHECK(table.entries()[0].description == "second");
    CHECK(table.entries()[1].description == "third");
    CHECK(table.entries()[2].description == "first");
    // The move keeps the identity and carries the selection with the row.
    CHECK(table.entries()[2].id == ids[0]);
    CHECK(table.entries()[0].id == ids[1]);
    CHECK(table.selected() == 2);

    // An equal, out-of-range from or out-of-range to is a no-op.
    table.move(1, 1);
    table.move(5, 0);
    table.move(0, 9);
    CHECK(table.entries()[0].description == "second");
    CHECK(table.entries()[1].description == "third");
    CHECK(table.entries()[2].description == "first");

    // A single row cannot be moved anywhere and stays put.
    AddressTable single;
    single.add(make_entry(kBase, ValueType::int32, {1, 0, 0, 0}));
    single.move(0, 0);
    REQUIRE(single.size() == 1);
}

TEST_CASE("merge appends with fresh ids and skips exact duplicates", "[table]")
{
    SECTION("appends with fresh ids and skips exact duplicates")
    {
        AddressTable table;
        auto         existing = make_entry(kBase, ValueType::int32, {1, 0, 0, 0});
        existing.description  = "health";
        existing.active       = true;
        table.add(existing);
        table.settings().target_process = "current";
        table.set_selected(0);
        const std::uint64_t existing_id = table.entries()[0].id;

        auto duplicate  = existing; // same address, type and description
        duplicate.bytes = {
            std::byte {9}, std::byte {9}, std::byte {9}, std::byte {9}}; // different bytes count as present

        auto new_address        = make_entry(kBase + 4, ValueType::int32, {2, 0, 0, 0});
        new_address.description = "mana";

        auto new_type        = make_entry(kBase, ValueType::float64, {});
        new_type.description = "health";

        auto new_text        = make_entry(kBase, ValueType::int32, {1, 0, 0, 0});
        new_text.description = "stamina";

        const std::vector<AddressEntry> incoming {duplicate, new_address, new_type, new_text};
        const auto                      summary = table.merge(incoming);

        CHECK(summary.added == 3);
        CHECK(summary.skipped == 1);
        REQUIRE(table.size() == 4);
        CHECK(table.entries()[0].id == existing_id);
        CHECK(table.entries()[0].bytes == existing.bytes); // the existing row wins
        CHECK(table.entries()[0].active);
        CHECK(table.entries()[1].description == "mana");
        CHECK(table.entries()[2].type == ValueType::float64);
        CHECK(table.entries()[3].description == "stamina");
        CHECK(table.entries()[1].id != existing_id);
        CHECK(table.selected() == 0); // selection untouched
        CHECK(table.settings().target_process == "current");
    }

    SECTION("adds an incoming duplicate only once and is idempotent")
    {
        AddressTable table;
        auto         row = make_entry(kBase, ValueType::int32, {1, 0, 0, 0});
        row.description  = "health";

        const std::vector<AddressEntry> incoming {row, row};
        const auto                      first = table.merge(incoming);
        CHECK(first.added == 1);
        CHECK(first.skipped == 1);
        REQUIRE(table.size() == 1);

        const auto second = table.merge(incoming);
        CHECK(second.added == 0);
        CHECK(second.skipped == 2);
        REQUIRE(table.size() == 1);
    }
}

TEST_CASE("entry mutations are recorded on the table category", "[table][log]")
{
    SECTION("entry mutations are recorded on the table category")
    {
        LevelGuard level;
        slopkit::log::Logger::instance().set_minimum_level(slopkit::log::Level::info);

        std::vector<slopkit::log::Record> records;
        SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                                {
                            records.push_back(record);
                                                }};

        AddressTable table;
        table.add(make_entry(kBase, ValueType::int32, {1, 0, 0, 0}));
        table.remove(0);

        bool saw_add    = false;
        bool saw_remove = false;
        for (const auto& record : records)
        {
            if (std::string_view {record.category} != slopkit::log::category::table
                || record.level != slopkit::log::Level::info)
            {
                continue;
            }
            saw_add    = saw_add || record.message.starts_with("entry added: ");
            saw_remove = saw_remove || record.message.starts_with("entry removed");
        }
        CHECK(saw_add);
        CHECK(saw_remove);
    }

    SECTION("a damaged archive is recorded with its member name")
    {
        LevelGuard level;
        slopkit::log::Logger::instance().set_minimum_level(slopkit::log::Level::warning);

        const auto path = scratch_file("malformed_log.skt");
        std::filesystem::remove(path);
        REQUIRE(
            write_archive(path,
                          std::vector<ArchiveMember> {
                              {       .name = "version.txt",                              .text = "slopkit-table 3\n"},
                              {.name = "entries/health.txt", .text = "type=i32 hex=0 size=4 frozen=1 expr=\"1234\"\n"},
                              {         .name = "index.txt",                           .text = "entries/health.txt\n"}
        })
                .has_value());

        std::vector<slopkit::log::Record> records;
        SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                                {
                            records.push_back(record);
                                                }};

        AddressTable table;
        CHECK_FALSE(slopkit::table::load(path, table).has_value());
        std::filesystem::remove(path);

        bool saw_warning = false;
        for (const auto& record : records)
        {
            if (record.level == slopkit::log::Level::warning && record.category == "table"
                && record.message.find("entries/health.txt") != std::string::npos)
            {
                saw_warning = true;
            }
        }
        CHECK(saw_warning);
    }
}

TEST_CASE("a script entry is stored as a lua member holding only the source", "[table]")
{
    AddressTable table;
    auto         health = make_entry(0x1234, ValueType::int32, {1, 0, 0, 0});
    health.description  = "health";
    health.expression   = "1234";
    table.add(health);

    const std::string source = "print('hi')\nreturn 1";
    CHECK(table.add_script("greet", source) == 1);

    auto armor        = make_entry(0x2008, ValueType::int32, {2, 0, 0, 0});
    armor.description = "armor";
    armor.expression  = "2008";
    table.add(armor);

    const auto path = scratch_file("script_layout.skt");
    std::filesystem::remove(path);
    REQUIRE(slopkit::table::save(path, table).has_value());

    CHECK(member_names(path)
          == std::vector<std::string> {
              "version.txt", "entries/health.txt", "entries/greet.lua", "entries/armor.txt", "index.txt"});
    CHECK(member_text(path, "index.txt")
          == std::optional<std::string> {"entries/health.txt\nentries/greet.lua\nentries/armor.txt\n"});
    // The body is the source and nothing else: no header and no added newline.
    CHECK(member_text(path, "entries/greet.lua") == std::optional<std::string> {source});
    CHECK(member_text(path, "version.txt") == std::optional<std::string> {"slopkit-table 4\n"});

    std::filesystem::remove(path);
}

TEST_CASE("a mixed table round-trips its scripts verbatim in row order", "[table]")
{
    AddressTable table;
    auto         health = make_entry(0x1234, ValueType::int32, {1, 0, 0, 0});
    health.description  = "health";
    health.expression   = "1234";
    table.add(health);

    const std::string source = "local\ttab = {1, 2}\nprint(\"quoted\", 'single')\nreturn tab\n";
    CHECK(table.add_script("helper", source) == 1);
    CHECK(table.add_script("", "") == 2); // an empty description and an empty body

    const auto path = scratch_file("mixed.skt");
    std::filesystem::remove(path);
    REQUIRE(slopkit::table::save(path, table).has_value());

    AddressTable loaded;
    REQUIRE(slopkit::table::load(path, loaded).has_value());

    REQUIRE(loaded.size() == 3);
    CHECK(loaded.entries()[0].kind == EntryKind::value);
    CHECK(loaded.entries()[0].description == "health");
    CHECK(loaded.entries()[1].kind == EntryKind::script);
    CHECK(loaded.entries()[1].description == "helper");
    CHECK(loaded.entries()[1].script == source);
    CHECK(loaded.entries()[2].kind == EntryKind::script);
    CHECK(loaded.entries()[2].description == "unnamed");
    CHECK(loaded.entries()[2].script.empty());

    // A drag-reorder round-trips in the new order.
    loaded.move(2, 0);
    REQUIRE(slopkit::table::save(path, loaded).has_value());

    AddressTable reordered;
    REQUIRE(slopkit::table::load(path, reordered).has_value());
    std::filesystem::remove(path);

    REQUIRE(reordered.size() == 3);
    const std::vector<std::string> descriptions {
        reordered.entries()[0].description, reordered.entries()[1].description, reordered.entries()[2].description};
    CHECK(descriptions == std::vector<std::string> {"unnamed", "health", "helper"});
    CHECK(reordered.entries()[2].script == source);
}

TEST_CASE("a version 3 archive still loads and is re-saved as version 4", "[table]")
{
    const auto path = scratch_file("legacy.skt");
    std::filesystem::remove(path);
    REQUIRE(write_archive(path,
                          std::vector<ArchiveMember> {
                              {       .name = "version.txt",                     .text = "slopkit-table 3\n"},
                              {.name = "entries/health.txt", .text = "type=i32 hex=0 size=4 expr=\"1234\"\n"},
                              {         .name = "index.txt",                  .text = "entries/health.txt\n"}
    })
                .has_value());

    AddressTable table;
    REQUIRE(slopkit::table::load(path, table).has_value());
    REQUIRE(table.size() == 1);
    CHECK(table.entries()[0].kind == EntryKind::value);
    CHECK(table.entries()[0].description == "health");

    REQUIRE(slopkit::table::save(path, table).has_value());
    CHECK(member_text(path, "version.txt") == std::optional<std::string> {"slopkit-table 4\n"});
    std::filesystem::remove(path);
}

TEST_CASE("a legacy type=all member loads as a 4-byte scan", "[table]")
{
    const auto path = scratch_file("legacy_all.skt");
    std::filesystem::remove(path);
    REQUIRE(write_archive(path,
                          std::vector<ArchiveMember> {
                              {       .name = "version.txt",                     .text = "slopkit-table 4\n"},
                              {.name = "entries/health.txt", .text = "type=all hex=0 size=4 expr=\"1234\"\n"},
                              {         .name = "index.txt",                  .text = "entries/health.txt\n"}
    })
                .has_value());

    AddressTable table;
    REQUIRE(slopkit::table::load(path, table).has_value());
    std::filesystem::remove(path);

    // The removed "All" entry was a 4-byte integer scan.
    REQUIRE(table.size() == 1);
    CHECK(table.entries()[0].type == ValueType::int32);
    CHECK(table.entries()[0].bytes.size() == 4);
}

TEST_CASE("script rows are edited through the table, not the value path", "[table]")
{
    AddressTable      table;
    const std::size_t row = table.add_script("helper", "return 1");

    REQUIRE(table.size() == 1);
    CHECK(row == 0);
    CHECK(table.entries()[0].kind == EntryKind::script);
    CHECK(table.entries()[0].description == "helper");
    CHECK(table.entries()[0].script == "return 1");
    CHECK(table.selected() == 0); // add_script selects the new row

    // A script row has no value text and cannot be encoded.
    CHECK(table.display_value(0).empty());
    const auto encoded = table.encode_value(0, "1");
    REQUIRE_FALSE(encoded.has_value());
    CHECK(encoded.error() == AccessError::invalid_argument);

    // The source and the description can be replaced.
    CHECK(table.set_script(0, "return 2"));
    CHECK(table.entries()[0].script == "return 2");
    CHECK(table.set_description(0, "renamed"));
    CHECK(table.entries()[0].description == "renamed");

    // A value row refuses a script edit; an out-of-range row refuses both.
    table.add(make_entry(0x1000, ValueType::int32, {0, 0, 0, 0}));
    CHECK_FALSE(table.set_script(1, "return 3"));
    CHECK_FALSE(table.set_script(9, "return 3"));
    CHECK_FALSE(table.set_description(9, "x"));

    // Even with the Active flag on and bytes cached, a script row is never a
    // freeze item.
    table.entries()[0].active = true;
    table.entries()[0].bytes  = {std::byte {0x42}};
    CHECK(table.freeze_items(0.0, 0.1).empty());
}

TEST_CASE("merge treats scripts by description and source text", "[table]")
{
    AddressTable table;
    CHECK(table.add_script("helper", "return 1") == 0);

    AddressEntry same;
    same.kind        = EntryKind::script;
    same.description = "helper";
    same.script      = "return 1";

    AddressEntry rewritten = same;
    rewritten.script       = "return 2";

    AddressEntry renamed = same;
    renamed.description  = "other";

    AddressEntry value; // same description, but a value entry is a different thing
    value.description = "helper";

    const std::vector<AddressEntry> incoming {same, rewritten, renamed, value};
    const auto                      summary = table.merge(incoming);

    CHECK(summary.added == 3);
    CHECK(summary.skipped == 1);
    REQUIRE(table.size() == 4);
    CHECK(table.entries()[0].script == "return 1");
    CHECK(table.entries()[1].script == "return 2");
    CHECK(table.entries()[2].description == "other");
    CHECK(table.entries()[3].kind == EntryKind::value);
}
