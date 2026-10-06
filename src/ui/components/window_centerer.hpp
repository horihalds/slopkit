#pragma once

#include <QObject>

class QWidget;

namespace slopkit::ui
{
    // Puts a dialog at the centre of the window that owns it: the filter moves it
    // whenever it becomes visible, so it opens centred even after the user moved
    // it around last time, and it is never remembered anywhere. The parent's
    // frame centre is the anchor; a requested position is only a hint, so a
    // compositor that owns placement (native Wayland) may place the window
    // itself. A centerer parented to the window it tracks needs no owning member.
    class WindowCenterer : public QObject
    {
        Q_OBJECT

    public:
        // `window` must be a top-level with a parent widget; a window without one
        // is never moved.
        explicit WindowCenterer(QWidget& window, QObject* parent = nullptr);

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override;

    private:
        void center_on_parent();

        QWidget& window_;
    };

} // namespace slopkit::ui
