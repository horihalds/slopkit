#include "ui/completion_notifier.hpp"

#include <QMetaObject>

namespace slopkit::ui
{

    CompletionNotifier::CompletionNotifier(QObject* parent) : QObject(parent) {}

    void CompletionNotifier::post() noexcept
    {
        // At most one queued event is in flight; the flag is cleared when that
        // event runs, so a completion posted during the drain queues another.
        if (pending_.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }

        QMetaObject::invokeMethod(
            this,
            [this]
            {
                pending_.store(false, std::memory_order_release);
                emit completionsAvailable();
            },
            Qt::QueuedConnection);
    }

} // namespace slopkit::ui
