#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <QMetaType>
#include <QMutex>
#include <QString>
#include <QThread>

#include "trainer/runtime/action.hpp"
#include "trainer/runtime/rows.hpp"

namespace slopkit::trainer::gui
{
    using Perform  = std::function<std::string(const Action&)>;
    using Step     = std::function<void()>;
    using Snapshot = std::function<std::shared_ptr<const std::vector<RowView>>()>;
    using Header   = std::function<QString()>;

    // The session's own thread: `perform`, `step` and every target call they make
    // happen here, and the GUI only ever sees the row snapshots it is handed.
    class SessionWorker : public QThread
    {
        Q_OBJECT

    public:
        SessionWorker(Perform       perform,
                      Step          step,
                      Snapshot      snapshot,
                      Header        header,
                      std::uint32_t interval_ms,
                      QObject*      parent = nullptr);

        ~SessionWorker() override;

        // Called from the GUI thread. A QThread object lives in the thread that
        // created it, so a slot on it would run on the GUI thread; the action is
        // queued here instead and a queued call wakes the worker's own loop.
        void submit(const Action& action);

        // Ends the loop and waits for it, so the session is gone before the
        // window that owns it is.
        void stop();

    signals:
        void
        rowsChanged(std::shared_ptr<const std::vector<RowView>> rows, const QString& header, const QString& message);

    protected:
        void run() override;

    private:
        void tick();
        void publish();

        Perform             perform_;
        Step                step_;
        Snapshot            snapshot_;
        Header              header_;
        int                 interval_ms_;
        std::atomic<bool>   stopping_ {false};
        std::atomic<bool>   wake_queued_ {false};
        mutable QMutex      mutex_;
        std::vector<Action> pending_;
        QObject*            inbox_ {};
        QString             message_;
    };

}

Q_DECLARE_METATYPE(std::shared_ptr<const std::vector<slopkit::trainer::RowView>>)
