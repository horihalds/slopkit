#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include "process/access_worker.hpp"
#include "scan/types.hpp"
#include "table/address_table.hpp"
#include "table/serializer.hpp"

namespace
{
    using slopkit::process::WriteItem;
    using slopkit::scan::ValueType;
    using slopkit::table::AddressEntry;
    using slopkit::table::AddressTable;

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
    CHECK(table.display_value(0) == "0x0000002A");
}

TEST_CASE("encode_value rejects malformed input and unknown indices", "[table]")
{
    AddressTable table;
    table.add(make_entry(kBase, ValueType::int32, {}));

    CHECK_FALSE(table.encode_value(0, "not a number").has_value());
    CHECK_FALSE(table.encode_value(9, "1").has_value());
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

TEST_CASE("an address table round-trips through the serializer", "[table]")
{
    AddressTable original;
    original.add(make_entry(0x1234, ValueType::int32, {0x40, 0x00, 0x00, 0x00}));
    original.entries()[0].description = "player health";
    original.entries()[0].active      = true;

    original.add(make_entry(0x100000, ValueType::float64, {0, 0, 0, 0, 0, 0, 0x10, 0x40}));
    original.entries()[1].description = "he said \"hi\"";
    original.entries()[1].hex         = true;

    const auto path = std::filesystem::temp_directory_path() / "slopkit_table_roundtrip.txt";
    std::filesystem::remove(path);

    REQUIRE(slopkit::table::save(path, original).has_value());

    AddressTable loaded;
    REQUIRE(slopkit::table::load(path, loaded).has_value());
    std::filesystem::remove(path);

    REQUIRE(loaded.size() == 2);
    CHECK(loaded.entries()[0].description == "player health");
    CHECK(loaded.entries()[0].address == 0x1234);
    CHECK(loaded.entries()[0].type == ValueType::int32);
    CHECK(loaded.entries()[0].active);
    CHECK_FALSE(loaded.entries()[0].hex);
    CHECK(loaded.entries()[0].bytes == original.entries()[0].bytes);

    CHECK(loaded.entries()[1].description == "he said \"hi\"");
    CHECK(loaded.entries()[1].address == 0x100000);
    CHECK(loaded.entries()[1].type == ValueType::float64);
    CHECK_FALSE(loaded.entries()[1].active);
    CHECK(loaded.entries()[1].hex);
    CHECK(loaded.entries()[1].bytes == original.entries()[1].bytes);

    // Ids are not persisted; the loader regenerates non-zero, unique ids.
    CHECK(loaded.entries()[0].id != 0);
    CHECK(loaded.entries()[1].id != 0);
    CHECK(loaded.entries()[0].id != loaded.entries()[1].id);
}

TEST_CASE("the serializer rejects malformed files", "[table]")
{
    const auto path = std::filesystem::temp_directory_path() / "slopkit_table_malformed.txt";
    std::filesystem::remove(path);

    {
        std::ofstream file(path);
        file << "slopkit-table 1\n";
        file << "nonsense line\n";
    }
    AddressTable table;
    CHECK_FALSE(slopkit::table::load(path, table).has_value());

    {
        std::ofstream file(path);
        file << "slopkit-table 1\n";
        file << "entry description=\"x\" address=zzz type=i32 value=00\n";
    }
    CHECK_FALSE(slopkit::table::load(path, table).has_value());

    {
        std::ofstream file(path);
        file << "slopkit-table 1\n";
        file << "entry type=i32 value=0\n"; // odd hex length
    }
    CHECK_FALSE(slopkit::table::load(path, table).has_value());

    std::filesystem::remove(path);

    CHECK_FALSE(slopkit::table::load("/nonexistent/slopkit/table.txt", table).has_value());
}
