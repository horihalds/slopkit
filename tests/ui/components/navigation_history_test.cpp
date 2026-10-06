#include <catch2/catch.hpp>

#include <cstdint>

#include "ui/components/navigation_history.hpp"

using slopkit::ui::components::NavigationHistory;

TEST_CASE("navigation history is empty until a jump is recorded", "[ui][navigation]")
{
    NavigationHistory history;
    CHECK_FALSE(history.can_go_back());
    CHECK_FALSE(history.back().has_value());

    CHECK(history.record(0x1000, 0x2000));
    CHECK(history.can_go_back());

    const auto previous = history.back();
    REQUIRE(previous.has_value());
    CHECK(*previous == 0x1000);
    CHECK_FALSE(history.can_go_back());
    CHECK_FALSE(history.back().has_value());
}

TEST_CASE("navigation history records nothing when the top does not move", "[ui][navigation]")
{
    NavigationHistory history;
    CHECK_FALSE(history.record(0x4000, 0x4000));
    CHECK_FALSE(history.can_go_back());
    CHECK_FALSE(history.back().has_value());
}

TEST_CASE("navigation history keeps the most recent 64 jumps", "[ui][navigation]")
{
    NavigationHistory   history;
    const std::uint64_t overflow = 10;
    for (std::uint64_t i = 0; i < NavigationHistory::limit + overflow; ++i)
    {
        REQUIRE(history.record(i, i + 1));
    }

    // The oldest ten jumps were dropped, so exactly 64 undos are possible and
    // they walk back from the newest.
    for (std::uint64_t i = 0; i < NavigationHistory::limit; ++i)
    {
        const auto previous = history.back();
        REQUIRE(previous.has_value());
        CHECK(*previous == NavigationHistory::limit + overflow - 1 - i);
    }
    CHECK_FALSE(history.can_go_back());
    CHECK_FALSE(history.back().has_value());
}

TEST_CASE("navigation history clear forgets every remembered jump", "[ui][navigation]")
{
    NavigationHistory history;
    REQUIRE(history.record(1, 2));
    REQUIRE(history.record(2, 3));
    REQUIRE(history.can_go_back());

    history.clear();
    CHECK_FALSE(history.can_go_back());
    CHECK_FALSE(history.back().has_value());
}
