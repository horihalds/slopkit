#include <catch2/catch.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/log.hpp"
#include "core/log_categories.hpp"

namespace
{
    using slopkit::log::Level;

    // Resets the process-wide logger before a case and restores the default
    // minimum level afterwards, so no state leaks between cases.
    struct LoggerState
    {
        LoggerState()
        {
            slopkit::log::Logger::instance().clear_history();
            slopkit::log::Logger::instance().set_minimum_level(Level::debug);
        }

        ~LoggerState()
        {
            slopkit::log::Logger::instance().set_minimum_level(Level::info);
        }
    };

    // Removes the registered sink even when a failing assertion unwinds the case.
    struct SinkGuard
    {
        slopkit::log::SinkId id {};

        ~SinkGuard()
        {
            slopkit::log::Logger::instance().remove_sink(id);
        }
    };

    std::string read_file(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }
} // namespace

TEST_CASE("The logger drops records below the minimum level", "[log]")
{
    LoggerState state;
    auto&       logger = slopkit::log::Logger::instance();
    logger.set_minimum_level(Level::warning);

    int       received = 0;
    SinkGuard sink {logger.add_sink(
        [&received](const slopkit::log::Record&)
        {
            ++received;
        })};

    slopkit::log::info("test", "below the level");
    REQUIRE(logger.history().empty());
    REQUIRE(received == 0);

    slopkit::log::warning("test", "at the level");
    REQUIRE(logger.history().size() == 1);
    REQUIRE(received == 1);
}

TEST_CASE("Log level names round-trip and junk is rejected", "[log]")
{
    REQUIRE(slopkit::log::level_name(Level::debug) == "debug");
    REQUIRE(slopkit::log::level_name(Level::info) == "info");
    REQUIRE(slopkit::log::level_name(Level::warning) == "warning");
    REQUIRE(slopkit::log::level_name(Level::error) == "error");

    for (const Level level : {Level::debug, Level::info, Level::warning, Level::error})
    {
        Level parsed = Level::error;
        REQUIRE(slopkit::log::parse_level(slopkit::log::level_name(level), parsed));
        REQUIRE(parsed == level);
    }

    Level parsed = Level::info;
    REQUIRE_FALSE(slopkit::log::parse_level("trace", parsed));
    REQUIRE_FALSE(slopkit::log::parse_level("", parsed));
}

TEST_CASE("Every sink receives a record and can be removed individually", "[log]")
{
    LoggerState state;
    auto&       logger = slopkit::log::Logger::instance();
    logger.set_minimum_level(Level::info);

    int       first  = 0;
    int       second = 0;
    SinkGuard first_guard {logger.add_sink(
        [&first](const slopkit::log::Record&)
        {
            ++first;
        })};
    SinkGuard second_guard {logger.add_sink(
        [&second](const slopkit::log::Record&)
        {
            ++second;
        })};

    slopkit::log::info("test", "both");
    REQUIRE(first == 1);
    REQUIRE(second == 1);

    logger.remove_sink(first_guard.id);
    slopkit::log::info("test", "only the second sink");
    REQUIRE(first == 1);
    REQUIRE(second == 2);
}

TEST_CASE("A throwing sink cannot escape the logger", "[log]")
{
    LoggerState state;
    auto&       logger = slopkit::log::Logger::instance();
    logger.set_minimum_level(Level::info);

    SinkGuard throwing {logger.add_sink(
        [](const slopkit::log::Record&)
        {
            throw std::runtime_error("boom");
        })};
    int       counted = 0;
    SinkGuard counting {logger.add_sink(
        [&counted](const slopkit::log::Record&)
        {
            ++counted;
        })};

    REQUIRE_NOTHROW(slopkit::log::error("test", "still delivered"));
    REQUIRE(counted == 1);
    REQUIRE(logger.history().size() == 1);
}

TEST_CASE("The in-memory history keeps only the newest records", "[log]")
{
    LoggerState state;
    auto&       logger = slopkit::log::Logger::instance();
    logger.set_minimum_level(Level::info);

    constexpr std::size_t extra = 100;
    for (std::size_t index = 0; index < slopkit::log::Logger::kHistoryLimit + extra; ++index)
    {
        slopkit::log::info("test", std::to_string(index));
    }

    const auto history = logger.history();
    REQUIRE(history.size() == slopkit::log::Logger::kHistoryLimit);
    REQUIRE(history.front().message == std::to_string(extra));
    REQUIRE(history.back().message == std::to_string(slopkit::log::Logger::kHistoryLimit + extra - 1));

    logger.clear_history();
    REQUIRE(logger.history().empty());
}

TEST_CASE("Concurrent log calls each produce exactly one record", "[log]")
{
    LoggerState state;
    auto&       logger = slopkit::log::Logger::instance();
    logger.set_minimum_level(Level::info);

    constexpr int            thread_count = 8;
    constexpr int            per_thread   = 200;
    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    for (int thread = 0; thread < thread_count; ++thread)
    {
        workers.emplace_back(
            [thread]
            {
                for (int index = 0; index < per_thread; ++index)
                {
                    slopkit::log::info("test", std::to_string(thread) + ":" + std::to_string(index));
                }
            });
    }
    for (auto& worker : workers)
    {
        worker.join();
    }

    REQUIRE(logger.history().size() == static_cast<std::size_t>(thread_count) * per_thread);
}

TEST_CASE("The rolling file sink rotates past its cap", "[log]")
{
    LoggerState state;
    auto&       logger = slopkit::log::Logger::instance();
    logger.set_minimum_level(Level::info);

    const auto      directory = std::filesystem::path(SLOPKIT_TMP_DIR) / "log_test";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    const auto path = directory / "slopkit.log";

    {
        SinkGuard sink {logger.add_sink(slopkit::log::rolling_file_sink(path, 20))};
        slopkit::log::info("test", "first record");
        slopkit::log::info("test", "second record");
    }

    auto backup = path;
    backup += ".1";
    REQUIRE(std::filesystem::exists(path));
    REQUIRE(std::filesystem::exists(backup));

    const auto backup_text  = read_file(backup);
    const auto current_text = read_file(path);
    REQUIRE(backup_text.find("first record") != std::string::npos);
    REQUIRE(current_text.find("second record") != std::string::npos);
    REQUIRE(current_text.find("first record") == std::string::npos);

    std::filesystem::remove_all(directory, error);
}

TEST_CASE("The info level hides debug-only detail", "[log]")
{
    LoggerState state;
    auto&       logger = slopkit::log::Logger::instance();

    std::vector<slopkit::log::Record> records;
    SinkGuard                         sink {logger.add_sink(
        [&records](const slopkit::log::Record& record)
        {
            records.push_back(record);
        })};

    logger.set_minimum_level(Level::info);
    slopkit::log::debug(slopkit::log::category::process, "job submitted");
    slopkit::log::info(slopkit::log::category::process, "attached");
    REQUIRE(records.size() == 1);
    REQUIRE(records.back().level == Level::info);
    REQUIRE(std::string_view {records.back().category} == slopkit::log::category::process);

    logger.set_minimum_level(Level::debug);
    slopkit::log::debug(slopkit::log::category::process, "job executed");
    REQUIRE(records.size() == 2);
    REQUIRE(records.back().level == Level::debug);
    REQUIRE(std::string_view {records.back().category} == slopkit::log::category::process);
}
