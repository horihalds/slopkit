#include <catch2/catch.hpp>

#include <filesystem>

#include "trainer/runtime/logging.hpp"

TEST_CASE("the verbose log sits beside the trainer file", "[trainer][logging]")
{
    CHECK(slopkit::trainer::verbose_log_path("practice.trainer") == "practice.trainer.log");
    CHECK(slopkit::trainer::verbose_log_path(std::filesystem::path {"/games"} / "practice.trainer")
          == std::filesystem::path {"/games"} / "practice.trainer.log");
    CHECK(slopkit::trainer::verbose_log_path("/games/practice") == "/games/practice.log");
}
