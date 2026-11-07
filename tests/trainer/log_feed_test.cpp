#include <catch2/catch.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "core/log.hpp"
#include "trainer/runtime/gui/log_feed.hpp"

namespace
{
    using slopkit::log::Level;
    using slopkit::trainer::gui::log_line;
    using slopkit::trainer::gui::LogFeed;

    struct LoggerState
    {
        Level previous;

        LoggerState() : previous(slopkit::log::Logger::instance().minimum_level()) {}

        ~LoggerState()
        {
            slopkit::log::Logger::instance().set_minimum_level(previous);
        }
    };
}

TEST_CASE("a log feed drains only the records logged since the last drain", "[trainer][log_feed]")
{
    LoggerState state;
    slopkit::log::Logger::instance().set_minimum_level(Level::debug);

    std::atomic<int> wakes {0};
    LogFeed          feed {[&wakes]
                           {
                      ++wakes;
                           }};

    slopkit::log::info("test", "first");
    slopkit::log::warning("test", "second");

    CHECK(wakes.load() == 2);

    const auto records = feed.take_new();
    REQUIRE(records.size() == 2);
    CHECK(records[0].message == "first");
    CHECK(records[1].message == "second");
    CHECK(feed.take_new().empty());
}

TEST_CASE("a log feed keeps only the newest records up to its limit", "[trainer][log_feed]")
{
    LoggerState state;
    slopkit::log::Logger::instance().set_minimum_level(Level::info);

    LogFeed feed {[] {}, 3};
    for (int index = 0; index < 6; ++index)
    {
        slopkit::log::info("test", std::to_string(index));
    }

    const auto records = feed.take_new();
    REQUIRE(records.size() == 3);
    CHECK(records[0].message == "3");
    CHECK(records[1].message == "4");
    CHECK(records[2].message == "5");
}

TEST_CASE("a log feed hides records below info", "[trainer][log_feed]")
{
    LoggerState state;
    slopkit::log::Logger::instance().set_minimum_level(Level::debug);

    std::atomic<int> wakes {0};
    LogFeed          feed {[&wakes]
                           {
                      ++wakes;
                           }};

    slopkit::log::debug("test", "noise");
    slopkit::log::info("test", "kept");
    slopkit::log::error("test", "kept too");

    CHECK(wakes.load() == 2);

    const auto records = feed.take_new();
    REQUIRE(records.size() == 2);
    CHECK(records[0].level == Level::info);
    CHECK(records[1].level == Level::error);
}

TEST_CASE("a destroyed log feed stops delivering", "[trainer][log_feed]")
{
    LoggerState state;
    slopkit::log::Logger::instance().set_minimum_level(Level::info);

    std::atomic<int> wakes {0};
    {
        LogFeed feed {[&wakes]
                      {
                          ++wakes;
                      }};
        slopkit::log::info("test", "before");
        CHECK(wakes.load() == 1);
    }

    slopkit::log::info("test", "after");
    CHECK(wakes.load() == 1);
}

TEST_CASE("a log feed accepts records from several threads", "[trainer][log_feed]")
{
    LoggerState state;
    slopkit::log::Logger::instance().set_minimum_level(Level::info);

    std::atomic<int>         wakes {0};
    LogFeed                  feed {[&wakes]
                                   {
                      ++wakes;
                                   }};
    constexpr int            thread_count = 4;
    constexpr int            per_thread   = 50;
    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    for (int index = 0; index < thread_count; ++index)
    {
        workers.emplace_back(
            [index]
            {
                for (int line = 0; line < per_thread; ++line)
                {
                    slopkit::log::info("test", std::to_string(index) + ":" + std::to_string(line));
                }
            });
    }
    for (auto& worker : workers)
    {
        worker.join();
    }

    CHECK(wakes.load() == thread_count * per_thread);
    CHECK(feed.take_new().size() == static_cast<std::size_t>(thread_count) * per_thread);
}

TEST_CASE("a log line names its time, level and message on one line", "[trainer][log_feed]")
{
    slopkit::log::Record record;
    record.level   = Level::warning;
    record.message = "first line\nsecond line";

    const auto line = log_line(record);

    REQUIRE(line.size() > 12);
    CHECK(line[2] == ':');
    CHECK(line[5] == ':');
    CHECK(line.find("  warning  ") != std::string::npos);
    CHECK(line.find("first line second line") != std::string::npos);
    CHECK(line.find('\n') == std::string::npos);
}
