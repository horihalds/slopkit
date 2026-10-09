#include <catch2/catch.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "script/symbols.hpp"

namespace
{
    using slopkit::expr::SymbolRef;
    using slopkit::script::SymbolApi;
    using slopkit::script::SymbolTable;
} // namespace

TEST_CASE("symbol table registers, overwrites and removes", "[script]")
{
    SymbolTable table;

    SECTION("a fresh name is stored with its value")
    {
        REQUIRE(table.set("hp", 0x1337).has_value());
        CHECK(table.size() == 1);
        CHECK(table.lookup("hp") == 0x1337);
    }

    SECTION("an overwrite keeps one entry and updates the value")
    {
        REQUIRE(table.set("hp", 1).has_value());
        REQUIRE(table.set("hp", 2).has_value());
        CHECK(table.size() == 1);
        CHECK(table.lookup("hp") == 2);
    }

    SECTION("the extremes round-trip")
    {
        REQUIRE(table.set("zero", 0).has_value());
        REQUIRE(table.set("max", 0xFFFFFFFFFFFFFFFFULL).has_value());
        CHECK(table.lookup("zero") == 0);
        CHECK(table.lookup("max") == 0xFFFFFFFFFFFFFFFFULL);
    }

    SECTION("removing a known name drops it and a second remove is a no-op")
    {
        REQUIRE(table.set("hp", 1).has_value());
        CHECK(table.remove("hp"));
        CHECK_FALSE(table.lookup("hp").has_value());
        CHECK(table.size() == 0);
        CHECK_FALSE(table.remove("hp"));
    }

    SECTION("removing a never-registered name is a no-op")
    {
        CHECK_FALSE(table.remove("missing"));
        CHECK(table.size() == 0);
    }
}

TEST_CASE("symbol table matches names case-insensitively", "[script]")
{
    SymbolTable table;

    REQUIRE(table.set("HP", 1).has_value());
    CHECK(table.lookup("hp") == 1);
    CHECK(table.lookup("Hp") == 1);

    SECTION("an overwrite through a different spelling keeps the first")
    {
        REQUIRE(table.set("hp", 2).has_value());
        CHECK(table.size() == 1);

        const std::vector<SymbolRef> snapshot = table.snapshot();
        REQUIRE(snapshot.size() == 1);
        CHECK(snapshot.at(0).name == "HP");
        CHECK(snapshot.at(0).value == 2);
    }

    SECTION("removing through a different spelling works")
    {
        CHECK(table.remove("hp"));
        CHECK(table.size() == 0);
    }
}

TEST_CASE("symbol table snapshot is an isolated copy", "[script]")
{
    SymbolTable table;
    REQUIRE(table.set("hp", 1).has_value());

    const std::vector<SymbolRef> snapshot = table.snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot.at(0).value == 1);

    REQUIRE(table.set("hp", 2).has_value());
    REQUIRE(table.set("mp", 3).has_value());
    CHECK(snapshot.size() == 1);
    CHECK(snapshot.at(0).value == 1);
}

TEST_CASE("symbol table rejects names the parser could never produce", "[script]")
{
    SymbolTable table;

    SECTION("an empty or blank name")
    {
        CHECK(table.set("", 1).error() == "the name must not be empty");
        CHECK(table.set("   ", 1).error() == "the name must not be blank");
    }

    SECTION("a name containing '+'")
    {
        CHECK(table.set("a+b", 1).error() == "the name must not contain '+'");
    }

    SECTION("a name starting with '#'")
    {
        CHECK(table.set("#hp", 1).error() == "the name must not start with '#'");
    }

    SECTION("a rejected name leaves the table untouched")
    {
        CHECK_FALSE(table.set("bad+name", 1).has_value());
        CHECK(table.size() == 0);
    }
}

TEST_CASE("symbol table api wraps set and remove", "[script]")
{
    SymbolTable     table;
    const SymbolApi api = table.api();

    REQUIRE(api.set("hp", 7).has_value());
    CHECK(table.lookup("hp") == 7);

    CHECK(api.set("a+b", 1).error() == "the name must not contain '+'");

    REQUIRE(api.remove("hp").has_value());
    CHECK_FALSE(table.lookup("hp").has_value());
    REQUIRE(api.remove("hp").has_value()); // an unknown name still succeeds
}

TEST_CASE("symbol table stays consistent under concurrent writes and snapshots", "[script]")
{
    SymbolTable table;

    constexpr int kRounds = 2000;

    std::atomic<bool> writer_failed {false};
    std::thread       writer(
        [&table, &writer_failed]
        {
            for (int i = 0; i < kRounds; ++i)
            {
                if (!table.set("hp", static_cast<std::uint64_t>(i)).has_value())
                {
                    writer_failed = true;
                }
                table.remove("unused");
            }
        });

    std::atomic<bool> reader_failed {false};
    std::thread       reader(
        [&table, &reader_failed]
        {
            for (int i = 0; i < kRounds; ++i)
            {
                const std::vector<SymbolRef> snapshot = table.snapshot();
                if (snapshot.size() > 1 || (!snapshot.empty() && snapshot.at(0).name != "hp"))
                {
                    reader_failed = true;
                }
                (void)table.size();
            }
        });

    writer.join();
    reader.join();

    CHECK_FALSE(writer_failed);
    CHECK_FALSE(reader_failed);
    CHECK(table.lookup("hp") == static_cast<std::uint64_t>(kRounds - 1));
}
