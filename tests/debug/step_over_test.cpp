#include <catch2/catch.hpp>

#include "debug/step_over.hpp"

using slopkit::debug::plan_step;

TEST_CASE("plan_step is a plain single step away from a trap", "[debug][step_over]")
{
    const auto no_trap = plan_step(0x1000, 0, 4);
    CHECK(no_trap.is_plain_step());
    CHECK(no_trap.resume_address == 0);
    CHECK(no_trap.resume_step_size == 0);

    const auto trap_elsewhere = plan_step(0x1000, 0x2000, 4);
    CHECK(trap_elsewhere.is_plain_step());
}

TEST_CASE("plan_step lifts a trap sitting at RIP", "[debug][step_over]")
{
    const auto trapped = plan_step(0x1000, 0x1000, 3);
    CHECK_FALSE(trapped.is_plain_step());
    CHECK(trapped.resume_address == 0x1000);
    CHECK(trapped.resume_step_size == 3);
}

TEST_CASE("plan_step falls back to one byte for an unknown instruction size", "[debug][step_over]")
{
    const auto fallback = plan_step(0x1000, 0x1000, 0);
    CHECK_FALSE(fallback.is_plain_step());
    CHECK(fallback.resume_address == 0x1000);
    CHECK(fallback.resume_step_size == 1);
}
