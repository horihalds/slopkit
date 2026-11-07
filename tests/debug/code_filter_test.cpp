#include <catch2/catch.hpp>

#include <cstdint>
#include <vector>

#include "debug/code_filter.hpp"

using slopkit::debug::CodeFilter;
using slopkit::debug::CodeFilterCandidate;
using slopkit::debug::CodeFilterState;

namespace
{
    [[nodiscard]] std::vector<CodeFilterCandidate> candidates(std::initializer_list<std::uint64_t> targets)
    {
        std::vector<CodeFilterCandidate> result;
        for (const std::uint64_t target : targets)
        {
            result.push_back(CodeFilterCandidate {.target = target});
        }
        return result;
    }
}

TEST_CASE("an idle filter covers nothing", "[debug][code-filter]")
{
    CodeFilter filter;
    CHECK(filter.state() == CodeFilterState::idle);
    CHECK(filter.candidates().empty());
    CHECK_FALSE(filter.covers(0x1000));
    CHECK_FALSE(filter.next_waiting().has_value());
    CHECK(filter.armed_count() == 0);
    CHECK(filter.waiting_count() == 0);
    CHECK(filter.executed_count() == 0);
}

TEST_CASE("recording a hit marks the candidate executed and frees its binding", "[debug][code-filter]")
{
    CodeFilter filter;
    filter.start(candidates({0x1000, 0x2000}));

    REQUIRE(filter.state() == CodeFilterState::filtering);
    REQUIRE(filter.waiting_count() == 2);
    CHECK(filter.next_waiting() == 0x1000);
    CHECK_FALSE(filter.covers(0x1000));

    filter.mark_armed(0x1000, 7);
    CHECK(filter.covers(0x1000));
    CHECK(filter.armed_count() == 1);
    CHECK(filter.waiting_count() == 1);
    CHECK(filter.next_waiting() == 0x2000);

    CHECK(filter.record(0x1000));
    CHECK(filter.executed_count() == 1);
    CHECK(filter.armed_count() == 0);
    CHECK_FALSE(filter.covers(0x1000));
    CHECK_FALSE(filter.record(0x1000));
    CHECK(filter.candidates()[0].executions == 1);
    CHECK(filter.candidates()[0].breakpoint_id == 0);
    CHECK(filter.next_waiting() == 0x2000);
}

TEST_CASE("a restored site covers and counts a second arrival", "[debug][code-filter]")
{
    CodeFilter filter;
    filter.start(candidates({0x1000}));
    filter.mark_armed(0x1000, 9);

    CHECK(filter.record(0x1000));
    filter.mark_restored(0x1000);
    CHECK(filter.covers(0x1000));
    CHECK(filter.armed_count() == 0);
    CHECK(filter.waiting_count() == 0);

    CHECK_FALSE(filter.record(0x1000));
    CHECK(filter.candidates()[0].executions == 2);
    CHECK(filter.candidates()[0].breakpoint_id == 0);
    CHECK(filter.candidates()[0].restored);
    CHECK(filter.executed_count() == 1);
}

TEST_CASE("keep executed drops the unexecuted half and keep not executed mirrors it", "[debug][code-filter]")
{
    CodeFilter filter;
    filter.start(candidates({0x1000, 0x2000, 0x3000}));
    filter.mark_armed(0x1000, 1);
    filter.mark_armed(0x2000, 2);
    filter.mark_armed(0x3000, 3);
    filter.record(0x1000);

    CHECK(filter.drop_unexecuted() == 2);
    CHECK(filter.visible_count() == 1);
    CHECK(filter.dropped_count() == 2);
    CHECK(filter.candidates()[0].visible);
    CHECK_FALSE(filter.candidates()[1].visible);
    CHECK(filter.executed_count() == 1);
    CHECK(filter.waiting_count() == 0);
    CHECK(filter.next_waiting() == std::nullopt);

    CHECK(filter.drop_unexecuted() == 0);
    CHECK(filter.drop_executed() == 1);
    CHECK(filter.visible_count() == 0);
    CHECK(filter.dropped_count() == 3);
}

TEST_CASE("resetting returns the filter to idle", "[debug][code-filter]")
{
    CodeFilter filter;
    filter.start(candidates({0x1000}));
    filter.mark_armed(0x1000, 4);
    filter.reset();

    CHECK(filter.state() == CodeFilterState::idle);
    CHECK(filter.candidates().empty());
    CHECK_FALSE(filter.covers(0x1000));
    CHECK(filter.dropped_count() == 0);
    CHECK(filter.reset_reason().isEmpty());
}

TEST_CASE("the reset reason survives until the next start", "[debug][code-filter]")
{
    CodeFilter filter;
    filter.start(candidates({0x1000}));
    CHECK(filter.reset_reason().isEmpty());

    filter.reset(QStringLiteral("Target exited."));
    CHECK(filter.reset_reason() == QStringLiteral("Target exited."));

    filter.start(candidates({0x2000}));
    CHECK(filter.reset_reason().isEmpty());
}
