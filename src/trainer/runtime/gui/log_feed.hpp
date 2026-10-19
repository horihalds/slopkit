#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.hpp"

namespace slopkit::trainer::gui
{
    [[nodiscard]] std::string log_line(const log::Record& record);

    // A window's view of the log: the sink runs on whatever thread logged, so it
    // only queues a copy and asks the UI thread to wake up; the window drains it
    // through take_new().
    class LogFeed
    {
    public:
        explicit LogFeed(std::function<void()> wake, std::size_t limit = 500);
        ~LogFeed();

        LogFeed(const LogFeed&)            = delete;
        LogFeed& operator=(const LogFeed&) = delete;
        LogFeed(LogFeed&&)                 = delete;
        LogFeed& operator=(LogFeed&&)      = delete;

        [[nodiscard]] std::vector<log::Record> take_new();

    private:
        log::SinkId             id_ {};
        std::function<void()>   wake_;
        std::mutex              mutex_;
        std::deque<log::Record> pending_;
        std::size_t             limit_;
    };
}
