#include "ui/log_notifier.hpp"

#include <QMetaObject>

namespace slopkit::ui
{

    LogNotifier::LogNotifier(QObject* parent) : QObject(parent)
    {
        sink_id_ = log::Logger::instance().add_sink(
            [this](const log::Record& record)
            {
                {
                    const std::lock_guard lock(mutex_);
                    pending_.push_back(record);
                }

                // At most one queued event is in flight; the flag is cleared when
                // that event runs, so a record posted during the drain queues
                // another wake-up.
                if (wake_queued_.exchange(true, std::memory_order_acq_rel))
                {
                    return;
                }
                QMetaObject::invokeMethod(
                    this,
                    [this]
                    {
                        wake_queued_.store(false, std::memory_order_release);
                        emit recordsAvailable();
                    },
                    Qt::QueuedConnection);
            });
    }

    LogNotifier::~LogNotifier()
    {
        log::Logger::instance().remove_sink(sink_id_);
    }

    std::vector<log::Record> LogNotifier::take()
    {
        const std::lock_guard    lock(mutex_);
        std::vector<log::Record> records;
        records.swap(pending_);
        return records;
    }

} // namespace slopkit::ui
