#include "trainer/runtime/gui/log_feed.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>

namespace slopkit::trainer::gui
{
    namespace
    {
        std::string timestamp(std::chrono::system_clock::time_point time)
        {
            const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
            std::tm           local {};
#if defined(_WIN32)
            localtime_s(&local, &seconds);
#else
            localtime_r(&seconds, &local);
#endif
            return std::format("{:02}:{:02}:{:02}", local.tm_hour, local.tm_min, local.tm_sec);
        }

        std::string one_line(std::string message)
        {
            for (char& character : message)
            {
                if (character == '\n' || character == '\r')
                {
                    character = ' ';
                }
            }
            return message;
        }
    }

    std::string log_line(const log::Record& record)
    {
        return std::format(
            "{}  {}  {}", timestamp(record.time), log::level_name(record.level), one_line(record.message));
    }

    LogFeed::LogFeed(std::function<void()> wake, std::size_t limit)
        : wake_(std::move(wake)), limit_(std::max<std::size_t>(limit, 1))
    {
        id_ = log::Logger::instance().add_sink(
            [this](const log::Record& record)
            {
                if (record.level < log::Level::info)
                {
                    return;
                }
                {
                    const std::lock_guard lock(mutex_);
                    pending_.push_back(record);
                    while (pending_.size() > limit_)
                    {
                        pending_.pop_front();
                    }
                }
                if (wake_)
                {
                    wake_();
                }
            });
    }

    LogFeed::~LogFeed()
    {
        // Taking the logger's lock waits out a sink already running, so the
        // captured this cannot be used after this returns.
        log::Logger::instance().remove_sink(id_);
    }

    std::vector<log::Record> LogFeed::take_new()
    {
        const std::lock_guard    lock(mutex_);
        std::vector<log::Record> records;
        records.reserve(pending_.size());
        while (!pending_.empty())
        {
            records.push_back(std::move(pending_.front()));
            pending_.pop_front();
        }
        return records;
    }
}
