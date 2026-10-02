#include <catch2/catch.hpp>

#include "core/version.hpp"

TEST_CASE("slopkit reports its version", "[version]")
{
    REQUIRE_FALSE(slopkit::version().empty());
    REQUIRE(slopkit::version() == "0.1.0");
}
