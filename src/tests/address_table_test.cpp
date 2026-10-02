#include <catch2/catch.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "process/access.hpp"
#include "process/types.hpp"
#include "scan/types.hpp"
#include "table/address_table.hpp"
#include "table/serializer.hpp"

namespace
{
    using slopkit::process::AccessError;
    using slopkit::process::AccessMethod;
    using slopkit::scan::ValueType;
    using slopkit::table::AddressEntry;
    using slopkit::table::AddressTable;

    // A tiny in-memory target at a fixed base address.
    class FakeBackend : public slopkit::process::SessionBackend
    {
    public:
        static constexpr std::uint64_t kBase = 0x1000;

        std::vector<std::byte> memory = std::vector<std::byte>(0x40);

        [[nodiscard]] slopkit::process::ProcessId pid() const noexcept override
        {
            return 7;
        }

        [[nodiscard]] std::string_view plugin_id() const noexcept override
        {
            return "fake";
        }

        [[nodiscard]] AccessMethod advertised_methods() const noexcept override
        {
            return AccessMethod::procfs_mem;
        }

        [[nodiscard]] AccessMethod last_method() const noexcept override
        {
            return AccessMethod::procfs_mem;
        }

        std::expected<std::vector<std::byte>, AccessError> read(std::uint64_t address, std::size_t size) override
        {
            if (address < kBase || address - kBase + size > memory.size())
            {
                return std::unexpected(AccessError::not_found);
            }
            const auto offset = static_cast<std::size_t>(address - kBase);
            return std::vector<std::byte>(memory.begin() + static_cast<std::ptrdiff_t>(offset),
                                          memory.begin() + static_cast<std::ptrdiff_t>(offset + size));
        }

        std::expected<std::size_t, AccessError> write(std::uint64_t address, std::span<const std::byte> data) override
        {
            if (address < kBase || address - kBase + data.size() > memory.size())
            {
                return std::unexpected(AccessError::not_found);
            }
            const auto offset = static_cast<std::size_t>(address - kBase);
            std::copy(data.begin(), data.end(), memory.begin() + static_cast<std::ptrdiff_t>(offset));
            return data.size();
        }

        std::expected<std::vector<slopkit::process::ModuleInfo>, AccessError> modules() override
        {
            return std::vector<slopkit::process::ModuleInfo> {};
        }

        std::expected<std::vector<slopkit::process::ThreadInfo>, AccessError> threads() override
        {
            return std::vector<slopkit::process::ThreadInfo> {};
        }

        std::expected<std::vector<slopkit::process::RegionInfo>, AccessError> regions() override
        {
            return std::vector<slopkit::process::RegionInfo> {};
        }
    };

    struct FakeSession
    {
        FakeBackend*              backend {};
        slopkit::process::Session session;

        FakeSession()
        {
            auto owned = std::make_unique<FakeBackend>();
            backend    = owned.get();
            session    = slopkit::process::Session {std::move(owned)};
        }

        [[nodiscard]] std::int32_t int32_at(std::size_t offset) const
        {
            std::int32_t value = 0;
            std::memcpy(&value, backend->memory.data() + offset, sizeof(value));
            return value;
        }
    };

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

TEST_CASE("write_value writes the parsed bytes and caches them", "[table]")
{
    FakeSession  target;
    AddressTable table;
    table.add(make_entry(FakeBackend::kBase, ValueType::int32, {}));

    REQUIRE(table.write_value(0, "1234", target.session).has_value());
    CHECK(target.int32_at(0) == 1234);
    REQUIRE(table.entries()[0].bytes.size() == 4);
    CHECK(table.display_value(0) == "1234");

    table.entries()[0].hex = true;
    REQUIRE(table.write_value(0, "0x2A", target.session).has_value());
    CHECK(target.int32_at(0) == 42);
    CHECK(table.display_value(0) == "0x0000002A");
}

TEST_CASE("write_value rejects malformed input and detached sessions", "[table]")
{
    FakeSession  target;
    AddressTable table;
    table.add(make_entry(FakeBackend::kBase, ValueType::int32, {}));

    CHECK_FALSE(table.write_value(0, "not a number", target.session).has_value());
    CHECK(target.int32_at(0) == 0);

    CHECK_FALSE(table.write_value(9, "1", target.session).has_value());

    slopkit::process::Session detached;
    CHECK_FALSE(table.write_value(0, "1", detached).has_value());
}

TEST_CASE("tick_freeze rewrites only the active entries and honours the interval", "[table]")
{
    FakeSession  target;
    AddressTable table;

    auto active   = make_entry(FakeBackend::kBase, ValueType::int32, {42, 0, 0, 0});
    active.active = true;
    table.add(active);

    auto inactive   = make_entry(FakeBackend::kBase + 4, ValueType::int32, {55, 0, 0, 0});
    inactive.active = false;
    table.add(inactive);

    const auto first = table.tick_freeze(target.session, 0.0, 0.1);
    REQUIRE(first.has_value());
    CHECK(*first == 1);
    CHECK(target.int32_at(0) == 42);
    CHECK(target.int32_at(4) == 0);

    // Inside the interval nothing is rewritten.
    const auto early = table.tick_freeze(target.session, 0.05, 0.1);
    REQUIRE(early.has_value());
    CHECK(*early == 0);

    // Once the target changes the frozen value, the next tick restores it.
    target.backend->memory[0] = std::byte {0};
    REQUIRE(table.tick_freeze(target.session, 0.2, 0.1).has_value());
    CHECK(target.int32_at(0) == 42);
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
