#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slopkit::log
{

    enum class Level
    {
        debug,
        info,
        warning,
        error,
    };

    [[nodiscard]] std::string_view level_name(Level level) noexcept;
    [[nodiscard]] bool             parse_level(std::string_view text, Level& out) noexcept;

    struct Record
    {
        Level                                 level {Level::info};
        std::string                           category; // "plugin", "scan", "worker", "ui", ...
        std::string                           message;
        std::chrono::system_clock::time_point time;
    };

    // Invoked for every accepted record on the logging thread while the logger's
    // lock is held: keep it short and never log from inside a sink.
    using Sink   = std::function<void(const Record&)>;
    using SinkId = std::uint64_t;

    // The process-wide logger: an ordered level filter, a bounded in-memory
    // history and a fan-out to every registered sink. All calls are thread-safe.
    class Logger
    {
    public:
        static Logger& instance();

        Logger(const Logger&)            = delete;
        Logger& operator=(const Logger&) = delete;
        Logger(Logger&&)                 = delete;
        Logger& operator=(Logger&&)      = delete;

        void                set_minimum_level(Level level) noexcept;
        [[nodiscard]] Level minimum_level() const noexcept;

        void log(Level level, std::string_view category, std::string_view message);

        [[nodiscard]] std::vector<Record> history() const;
        void                              clear_history();

        SinkId add_sink(Sink sink);
        void   remove_sink(SinkId id) noexcept;

        static constexpr std::size_t kHistoryLimit = 2000;

    private:
        Logger() = default;

        mutable std::mutex                   mutex_;
        std::deque<Record>                   history_;
        std::vector<std::pair<SinkId, Sink>> sinks_;
        SinkId                               next_sink_id_ {1};
        Level                                minimum_ {Level::info};
    };

    void debug(std::string_view category, std::string_view message);
    void info(std::string_view category, std::string_view message);
    void warning(std::string_view category, std::string_view message);
    void error(std::string_view category, std::string_view message);

    [[nodiscard]] Sink                  stderr_sink();
    [[nodiscard]] Sink                  rolling_file_sink(std::filesystem::path path, std::size_t max_bytes);
    [[nodiscard]] std::filesystem::path default_log_path(); // $XDG_STATE_HOME or ~/.local/state

} // namespace slopkit::log
