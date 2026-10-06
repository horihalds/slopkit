#include "ui/components/window_geometry.hpp"

#include <format>

#include <QEvent>
#include <QWidget>

#include "core/log.hpp"
#include "core/log_categories.hpp"

namespace slopkit::ui
{
    WindowGeometryKeeper::WindowGeometryKeeper(QWidget&            window,
                                               SettingsController& settings,
                                               WindowId            id,
                                               QObject*            parent)
        : QObject(parent), window_(window), settings_(settings), id_(id)
    {
        // Restoring while the window is built (rather than on its first show)
        // means it never flashes at its default size before the saved one lands.
        const QByteArray saved = settings_.window_geometry(id_);
        if (!saved.isEmpty() && !window_.restoreGeometry(saved))
        {
            log::warning(log::category::app,
                         std::format("saved geometry for window {} could not be applied", static_cast<int>(id_)));
        }
        window_.installEventFilter(this);
    }

    bool WindowGeometryKeeper::eventFilter(QObject* watched, QEvent* event)
    {
        if (watched == &window_ && (event->type() == QEvent::Hide || event->type() == QEvent::Close))
        {
            // Idempotent, so a close followed by a hide writes the same blob twice.
            settings_.set_window_geometry(id_, window_.saveGeometry());
        }
        return QObject::eventFilter(watched, event);
    }
} // namespace slopkit::ui
