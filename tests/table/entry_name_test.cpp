#include <catch2/catch.hpp>

#include <optional>
#include <string>
#include <vector>

#include "table/entry_name.hpp"

namespace
{
    using slopkit::table::entry_member_name;
    using slopkit::table::EntryKind;
    using slopkit::table::kind_for_member;
    using slopkit::table::member_stem;
} // namespace

TEST_CASE("an entry member name is derived from its description", "[table]")
{
    const std::vector<std::string> none;
    CHECK(entry_member_name("health", none) == "health.txt");
    CHECK(entry_member_name("player health", none) == "player health.txt");

    // Nothing usable falls back to "unnamed".
    CHECK(entry_member_name("", none) == "unnamed.txt");
    CHECK(entry_member_name("   ", none) == "unnamed.txt");
    CHECK(entry_member_name(".", none) == "unnamed.txt");
    CHECK(entry_member_name("..", none) == "unnamed.txt");

    // Trailing dots and whitespace are dropped.
    CHECK(entry_member_name("health.", none) == "health.txt");
    CHECK(entry_member_name("health..  ", none) == "health.txt");
}

TEST_CASE("path-hostile characters are sanitised to underscores", "[table]")
{
    const std::vector<std::string> none;
    CHECK(entry_member_name("hp/max", none) == "hp_max.txt");
    CHECK(entry_member_name("a\\b", none) == "a_b.txt");
    CHECK(entry_member_name("q\"x", none) == "q_x.txt");
    CHECK(entry_member_name("x:y", none) == "x_y.txt");
    CHECK(entry_member_name("a*b?c<d>e|f", none) == "a_b_c_d_e_f.txt");
    CHECK(entry_member_name("ctrl" + std::string(1, '\x01') + "name", none) == "ctrl_name.txt");
}

TEST_CASE("a name collision gets the smallest free number", "[table]")
{
    std::vector<std::string> taken;
    CHECK(entry_member_name("unnamed", taken) == "unnamed.txt");
    taken.push_back("unnamed.txt");
    CHECK(entry_member_name("", taken) == "unnamed2.txt");
    taken.push_back("unnamed2.txt");
    CHECK(entry_member_name("", taken) == "unnamed3.txt");

    // The first free number wins, not just the next one.
    taken.clear();
    taken.push_back("health.txt");
    taken.push_back("health3.txt");
    CHECK(entry_member_name("health", taken) == "health2.txt");
}

TEST_CASE("a very long description is capped", "[table]")
{
    const std::vector<std::string> none;
    const std::string              name = entry_member_name(std::string(200, 'a'), none);
    CHECK(name.size() <= 100);
    CHECK(name.ends_with(".txt"));
    CHECK(name == std::string(96, 'a') + ".txt");
}

TEST_CASE("a script entry member name uses the lua extension", "[table]")
{
    const std::vector<std::string> none;
    CHECK(entry_member_name("greet", none, ".lua") == "greet.lua");
    CHECK(entry_member_name("", none, ".lua") == "unnamed.lua");

    // The numbering is per extension, so a value entry and a script entry may
    // share a stem.
    std::vector<std::string> taken;
    CHECK(entry_member_name("health", taken, ".txt") == "health.txt");
    taken.push_back("health.txt");
    CHECK(entry_member_name("health", taken, ".lua") == "health.lua");
    taken.push_back("health.lua");
    CHECK(entry_member_name("health", taken, ".lua") == "health2.lua");
}

TEST_CASE("a member name classifies as a kind and yields its stem", "[table]")
{
    CHECK(kind_for_member("entries/health.txt") == EntryKind::value);
    CHECK(kind_for_member("entries/greet.lua") == EntryKind::script);
    CHECK(kind_for_member("entries/health") == std::nullopt);
    CHECK(kind_for_member("entries/notes.md") == std::nullopt);

    CHECK(member_stem("entries/health.txt") == "health");
    CHECK(member_stem("entries/greet.lua") == "greet");
    CHECK(member_stem("entries/plain") == "plain");
    CHECK(member_stem("health.txt") == "health");
}
