#pragma once

#include <atomic>

#include <QObject>

namespace slopkit::ui
{

    // Bridges the access worker's thread to the Qt event loop. The worker's
    // completion hook calls post() on the worker thread; the class coalesces
    // repeated posts into at most one queued event and then emits
    // completionsAvailable() on the UI thread.
    class CompletionNotifier : public QObject
    {
        Q_OBJECT

    public:
        explicit CompletionNotifier(QObject* parent = nullptr);

        // Thread-safe; may be called from any thread. Never touches a widget.
        void post() noexcept;

    signals:
        void completionsAvailable();

    private:
        // True while a wake-up is queued; keeps post() coalescing.
        std::atomic<bool> pending_ {false};
    };

} // namespace slopkit::ui
