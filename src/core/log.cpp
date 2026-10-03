#include "core/log.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <format>
#include <fstream>
#include <memory>
#include <system_error>

namespace slopkit::log
{
    namespace
    {
        std::string format_timestamp(std::chrono::system_clock::time_point time)
        {
            const auto since_epoch    = std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch());
            const std::time_t seconds = std::chrono::system_clock::to_time_t(time);

            std::tm local {};
            localtime_r(&seconds, &local);

            long milliseconds = static_cast<long>(since_epoch.count() % 1000);
            if (milliseconds < 0)
            {
                milliseconds += 1000;
            }
            return std::format("{:02}:{:02}:{:02}.{:03}", local.tm_hour, local.tm_min, local.tm_sec, milliseconds);
        }

        std::string format_record(const Record& record)
        {
            return std::format("{} {} [{}] {}\n",
                               format_timestamp(record.time),
                               level_name(record.level),
                               record.category,
                               record.message);
        }

        void report_sink_failure(const char* message)
        {
            std::fputs(message, stderr);
        }

        struct FileSinkState
        {
            std::filesystem::path path;
            std::size_t           max_bytes {0};
            std::ofstream         stream;
            std::uintmax_t        size {0};
            bool                  failed {false};
        };

        void rotate_file(const std::shared_ptr<FileSinkState>& state)
        {
            state->stream.close();

            std::error_code       error;
            std::filesystem::path backup = state->path;
            backup += ".1";
            std::filesystem::remove(backup, error);
            std::filesystem::rename(state->path, backup, error);

            state->stream.open(state->path, std::ios::binary | std::ios::trunc);
            state->size = 0;
            if (!state->stream)
            {
                state->failed = true;
                report_sink_failure("slopkit: the log file could not be rotated; continuing with stderr only\n");
            }
        }
    } // namespace

    std::string_view level_name(Level level) noexcept
    {
        switch (level)
        {
        case Level::debug:
            return "debug";
        case Level::info:
            return "info";
        case Level::warning:
            return "warning";
        case Level::error:
            return "error";
        }
        std::unreachable();
    }

    bool parse_level(std::string_view text, Level& out) noexcept
    {
        if (text == "debug")
        {
            out = Level::debug;
            return true;
        }
        if (text == "info")
        {
            out = Level::info;
            return true;
        }
        if (text == "warning" || text == "warn")
        {
            out = Level::warning;
            return true;
        }
        if (text == "error")
        {
            out = Level::error;
            return true;
        }
        return false;
    }

    Logger& Logger::instance()
    {
        static Logger logger;
        return logger;
    }

    void Logger::set_minimum_level(Level level) noexcept
    {
        const std::lock_guard lock(mutex_);
        minimum_ = level;
    }

    Level Logger::minimum_level() const noexcept
    {
        const std::lock_guard lock(mutex_);
        return minimum_;
    }

    void Logger::log(Level level, std::string_view category, std::string_view message)
    {
        const std::lock_guard lock(mutex_);
        if (level < minimum_)
        {
            return;
        }

        Record record;
        record.level = level;
        record.category.assign(category);
        record.message.assign(message);
        record.time = std::chrono::system_clock::now();

        history_.push_back(record);
        while (history_.size() > kHistoryLimit)
        {
            history_.pop_front();
        }

        for (const auto& [id, sink] : sinks_)
        {
            (void)id;
            try
            {
                sink(record);
            }
            catch (...)
            {
                // A sink must never take the application down with it.
            }
        }
    }

    std::vector<Record> Logger::history() const
    {
        const std::lock_guard lock(mutex_);
        return {history_.begin(), history_.end()};
    }

    void Logger::clear_history()
    {
        const std::lock_guard lock(mutex_);
        history_.clear();
    }

    SinkId Logger::add_sink(Sink sink)
    {
        const std::lock_guard lock(mutex_);
        const SinkId          id = next_sink_id_++;
        sinks_.emplace_back(id, std::move(sink));
        return id;
    }

    void Logger::remove_sink(SinkId id) noexcept
    {
        const std::lock_guard lock(mutex_);
        std::erase_if(sinks_,
                      [id](const auto& entry)
                      {
                          return entry.first == id;
                      });
    }

    void debug(std::string_view category, std::string_view message)
    {
        Logger::instance().log(Level::debug, category, message);
    }

    void info(std::string_view category, std::string_view message)
    {
        Logger::instance().log(Level::info, category, message);
    }

    void warning(std::string_view category, std::string_view message)
    {
        Logger::instance().log(Level::warning, category, message);
    }

    void error(std::string_view category, std::string_view message)
    {
        Logger::instance().log(Level::error, category, message);
    }

    Sink stderr_sink()
    {
        return [](const Record& record)
        {
            const std::string line = format_record(record);
            std::fwrite(line.data(), 1, line.size(), stderr);
            std::fflush(stderr);
        };
    }

    Sink rolling_file_sink(std::filesystem::path path, std::size_t max_bytes)
    {
        auto state       = std::make_shared<FileSinkState>();
        state->path      = std::move(path);
        state->max_bytes = max_bytes;

        return [state](const Record& record)
        {
            if (state->failed)
            {
                return;
            }

            if (!state->stream.is_open())
            {
                std::error_code error;
                if (state->path.has_parent_path() && !state->path.parent_path().empty())
                {
                    std::filesystem::create_directories(state->path.parent_path(), error);
                }
                state->stream.open(state->path, std::ios::binary | std::ios::app);
                if (!state->stream)
                {
                    state->failed = true;
                    report_sink_failure("slopkit: the log file could not be opened; continuing with stderr only\n");
                    return;
                }
                const auto size = std::filesystem::file_size(state->path, error);
                state->size     = error ? 0 : size;
            }

            if (state->max_bytes > 0 && state->size >= state->max_bytes)
            {
                rotate_file(state);
                if (state->failed)
                {
                    return;
                }
            }

            const std::string line = format_record(record);
            state->stream.write(line.data(), static_cast<std::streamsize>(line.size()));
            if (!state->stream)
            {
                state->failed = true;
                report_sink_failure("slopkit: writing the log file failed; continuing with stderr only\n");
                return;
            }
            state->size += line.size();
        };
    }

    std::filesystem::path default_log_path()
    {
        std::filesystem::path base;
        if (const char* state_home = std::getenv("XDG_STATE_HOME"); state_home != nullptr && *state_home != '\0')
        {
            base = state_home;
        }
        else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
        {
            base = std::filesystem::path(home) / ".local" / "state";
        }
        else
        {
            return std::filesystem::path("slopkit.log");
        }

        return base / "slopkit" / "slopkit.log";
    }

} // namespace slopkit::log
