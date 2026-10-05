#include <catch2/catch.hpp>

#include <cstdint>

#include "debug/access_watch.hpp"

using slopkit::debug::AccessWatch;
using slopkit::debug::Kind;
using slopkit::debug::WatchState;

TEST_CASE("an access watch records one row per instruction with a count", "[debug][access_watch]")
{
    AccessWatch watch;
    CHECK(watch.state() == WatchState::idle);

    watch.start(0x4000, Kind::hardware_write, 4, 7);
    CHECK(watch.state() == WatchState::watching);
    CHECK(watch.address() == 0x4000);
    CHECK(watch.kind() == Kind::hardware_write);
    CHECK(watch.size() == 4);
    CHECK(watch.breakpoint_id() == 7);
    CHECK(watch.hits().empty());
    CHECK(watch.hit_count() == 0);

    watch.record(11, 0x5005);
    watch.record(11, 0x5005);
    watch.record(12, 0x6000);

    REQUIRE(watch.hits().size() == 2);
    CHECK(watch.hits()[0].instruction == 0x5005);
    CHECK(watch.hits()[0].tid == 11);
    CHECK(watch.hits()[0].count == 2);
    CHECK(watch.hits()[1].instruction == 0x6000);
    CHECK(watch.hits()[1].count == 1);
    CHECK(watch.hit_count() == 3);
    CHECK_FALSE(watch.truncated());
}

TEST_CASE("an access watch stops growing rows at the cap but keeps counting", "[debug][access_watch]")
{
    AccessWatch watch;
    watch.start(0x4000, Kind::hardware_read_write, 8, 1);

    for (std::uint64_t address = 0; address < AccessWatch::kMaxHits; ++address)
    {
        watch.record(1, 0x1000 + address);
    }
    CHECK(watch.hits().size() == AccessWatch::kMaxHits);
    CHECK_FALSE(watch.truncated());

    // The 1025th distinct instruction only flips the truncation flag; its hit is
    // still counted in the total.
    watch.record(1, 0x9000);
    CHECK(watch.hits().size() == AccessWatch::kMaxHits);
    CHECK(watch.truncated());
    CHECK(watch.hit_count() == AccessWatch::kMaxHits + 1);
}

TEST_CASE("stopping an access watch keeps the rows and starting a new one drops them", "[debug][access_watch]")
{
    AccessWatch watch;
    watch.start(0x4000, Kind::hardware_write, 4, 1);
    watch.record(1, 0x5000);

    watch.stop();
    CHECK(watch.state() == WatchState::stopped);
    CHECK(watch.hits().size() == 1);

    watch.clear_hits();
    CHECK(watch.hits().empty());
    CHECK(watch.hit_count() == 0);

    watch.start(0x8000, Kind::hardware_read_write, 2, 2);
    CHECK(watch.state() == WatchState::watching);
    CHECK(watch.address() == 0x8000);
    CHECK(watch.hit_count() == 0);
    CHECK(watch.hits().empty());
}

TEST_CASE("a failed arming ends an access watch", "[debug][access_watch]")
{
    AccessWatch watch;
    watch.start(0x4000, Kind::hardware_write, 4, 1);

    watch.mark_armed(false);
    CHECK(watch.state() == WatchState::stopped);

    watch.start(0x4000, Kind::hardware_write, 4, 1);
    watch.mark_armed(true);
    CHECK(watch.state() == WatchState::watching);
}
