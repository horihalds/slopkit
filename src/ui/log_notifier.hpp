#pragma once

#include <atomic>
#include <mutex>
#include <vector>

#include <QObject>

#include "core/log.hpp"

namespace slopkit::ui
{
    class LogNotifier : public QObject
    {
        Q_OBJECT

    public:
        explicit LogNotifier(QObject* parent = nullptr);
        ~LogNotifier() override;

        [[nodiscard]] std::vector<log::Record> take();

    signals:
        void recordsAvailable();

    private:
        std::mutex               mutex_;
        std::vector<log::Record> pending_;
        std::atomic<bool>        wake_queued_ {false};
        log::SinkId              sink_id_ {};
    };

}
