#include "ui/completion_notifier.hpp"

#include <QMetaObject>

namespace slopkit::ui
{
    CompletionNotifier::CompletionNotifier(QObject* parent) : QObject(parent) {}

    void CompletionNotifier::post() noexcept
    {
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

}
