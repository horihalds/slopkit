#pragma once

#include <atomic>
#include <mutex>
#include <vector>

#include <QObject>

#include "core/log.hpp"

namespace slopkit::ui
{

    // Copies log records from any thread into a queue and posts one coalesced
    // queued wake-up; the UI thread drains it through take(). It registers a
    // logger sink in its constructor and removes it again when destroyed.
    class LogNotifier : public QObject
    {
        Q_OBJECT

    public:
        explicit LogNotifier(QObject* parent = nullptr);
        ~LogNotifier() override;

        // Thread-safe; returns and clears the records queued since the last call.
        [[nodiscard]] std::vector<log::Record> take();

    signals:
        void recordsAvailable();

    private:
        std::mutex               mutex_;
        std::vector<log::Record> pending_;
        std::atomic<bool>        wake_queued_ {false};
        log::SinkId              sink_id_ {};
    };

} // namespace slopkit::ui
