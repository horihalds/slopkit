#include "ui/components/window_centerer.hpp"

#include <QEvent>
#include <QWidget>

namespace slopkit::ui
{
    WindowCenterer::WindowCenterer(QWidget& window, QObject* parent)
        : QObject(parent != nullptr ? parent : &window), window_(window)
    {
        window_.installEventFilter(this);
    }

    bool WindowCenterer::eventFilter(QObject* watched, QEvent* event)
    {
        if (watched == &window_ && event->type() == QEvent::Show)
        {
            center_on_parent();
        }
        return QObject::eventFilter(watched, event);
    }

    void WindowCenterer::center_on_parent()
    {
        const QWidget* parent = window_.parentWidget();
        if (parent == nullptr || !window_.isWindow())
        {
            return;
        }
        const QSize own = window_.size();
        window_.move(parent->frameGeometry().center() - QPoint(own.width() / 2, own.height() / 2));
    }
} // namespace slopkit::ui
