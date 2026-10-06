#pragma once

#include <QByteArray>
#include <QString>

namespace slopkit::ui
{

    // The `QT_QPA_PLATFORM` value a GUI run should use so that a remembered
    // window frame can be restored. A Wayland compositor owns a top-level
    // window's position and drops a requested one, so X11/XWayland (`xcb`) is
    // preferred when a `DISPLAY` is available, with native Wayland kept as the
    // fallback for a session where the xcb platform plugin cannot load.
    // An explicit `configured` value always wins, and an empty result means
    // "leave Qt's own default" (native Wayland).
    [[nodiscard]] QString preferred_platform_spec(const QByteArray& configured, const QByteArray& display);

    // Applies preferred_platform_spec() through qputenv, so this process and the
    // children it launches share one platform, and records the choice once.
    // Never overwrites an explicitly configured QT_QPA_PLATFORM. Call before the
    // QApplication is built.
    void prefer_platform_with_remembered_positions();

} // namespace slopkit::ui
