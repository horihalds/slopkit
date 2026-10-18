#include "trainer/runtime/gui/session_worker.hpp"

#include <utility>

#include <QMetaObject>
#include <QMutexLocker>
#include <QTimer>

namespace slopkit::trainer::gui
{
    SessionWorker::SessionWorker(
        Perform perform, Step step, Snapshot snapshot, Header header, std::uint32_t interval_ms, QObject* parent)
        : QThread(parent), perform_(std::move(perform)), step_(std::move(step)), snapshot_(std::move(snapshot)),
          header_(std::move(header)), interval_ms_(static_cast<int>(interval_ms))
    {
        // The row snapshot crosses from this thread to the GUI, so its type has
        // to be known to the queued connection before the first emission.
        qRegisterMetaType<std::shared_ptr<const std::vector<RowView>>>(
            "std::shared_ptr<const std::vector<slopkit::trainer::RowView>>");
        qRegisterMetaType<Action>("slopkit::trainer::Action");
    }

    SessionWorker::~SessionWorker()
    {
        stop();
    }

    void SessionWorker::submit(const Action& action)
    {
        if (stopping_.load())
        {
            return;
        }

        QMutexLocker lock(&mutex_);
        pending_.push_back(action);
        if (inbox_ != nullptr && !wake_queued_.exchange(true))
        {
            QMetaObject::invokeMethod(
                inbox_,
                [this]
                {
                    wake_queued_.store(false);
                    tick();
                },
                Qt::QueuedConnection);
        }
    }

    void SessionWorker::stop()
    {
        stopping_.store(true);
        if (isRunning())
        {
            QMutexLocker lock(&mutex_);
            if (inbox_ != nullptr)
            {
                QMetaObject::invokeMethod(
                    inbox_,
                    [this]
                    {
                        quit();
                    },
                    Qt::QueuedConnection);
            }
        }
        wait();
    }

    void SessionWorker::run()
    {
        // Created here, so the timer and the queued wake-ups both belong to this
        // thread and no action can run on the GUI thread.
        QObject inbox;
        {
            QMutexLocker lock(&mutex_);
            inbox_ = &inbox;
        }

        QTimer timer;
        timer.setInterval(interval_ms_);
        QObject::connect(&timer,
                         &QTimer::timeout,
                         &inbox,
                         [this]
                         {
                             tick();
                         });
        timer.start();

        if (!stopping_.load())
        {
            // The first pass fills the window before the user sees it, and a
            // target that is not running yet simply leaves the rows unresolved.
            tick();
            exec();
        }

        {
            QMutexLocker lock(&mutex_);
            inbox_ = nullptr;
        }
    }

    void SessionWorker::tick()
    {
        if (stopping_.load())
        {
            return;
        }

        std::vector<Action> actions;
        {
            QMutexLocker lock(&mutex_);
            actions.swap(pending_);
        }
        for (const auto& action : actions)
        {
            const auto message = perform_(action);
            if (!message.empty())
            {
                message_ = QString::fromStdString(message);
            }
        }

        step_();
        publish();
    }

    void SessionWorker::publish()
    {
        emit rowsChanged(snapshot_(), header_(), message_);
    }

}
