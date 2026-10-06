#include "ui/platform.hpp"

#include <format>
#include <string>

#include <QtGlobal>

#include "core/log.hpp"
#include "core/log_categories.hpp"

namespace slopkit::ui
{
    QString preferred_platform_spec(const QByteArray& configured, const QByteArray& display)
    {
        // An explicit choice (a user, a shell or the test suite) always wins.
        if (!configured.isEmpty())
        {
            return QString::fromLocal8Bit(configured);
        }
        // Without an X11 display there is nothing to prefer; leave Qt's default
        // (native Wayland) in place.
        if (display.isEmpty())
        {
            return QString {};
        }
        return QStringLiteral("xcb;wayland");
    }

    void prefer_platform_with_remembered_positions()
    {
        const QByteArray configured = qgetenv("QT_QPA_PLATFORM");
        const QString    spec       = preferred_platform_spec(configured, qgetenv("DISPLAY"));

        if (!spec.isEmpty() && spec.toLocal8Bit() != configured)
        {
            qputenv("QT_QPA_PLATFORM", spec.toLocal8Bit());
        }

        const std::string chosen = spec.isEmpty() ? std::string {"Qt's default"} : spec.toStdString();
        log::info(log::category::ui, std::format("GUI platform for this run: {}", chosen));
    }
} // namespace slopkit::ui
