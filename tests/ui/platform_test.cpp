#include <catch2/catch.hpp>

#include "ui/platform.hpp"

TEST_CASE("a GUI run prefers the platform that can restore a position", "[platform]")
{
    using slopkit::ui::preferred_platform_spec;

    // No explicit choice and a display: prefer xcb, keep wayland as a fallback.
    CHECK(preferred_platform_spec(QByteArray {}, QByteArray {":0"}) == QStringLiteral("xcb;wayland"));

    // An explicit choice is never replaced.
    CHECK(preferred_platform_spec(QByteArray {"offscreen"}, QByteArray {":0"}) == QStringLiteral("offscreen"));
    CHECK(preferred_platform_spec(QByteArray {"wayland"}, QByteArray {":0"}) == QStringLiteral("wayland"));
    CHECK(preferred_platform_spec(QByteArray {"wayland"}, QByteArray {}) == QStringLiteral("wayland"));

    // No display: leave Qt's own default (native Wayland) in place.
    CHECK(preferred_platform_spec(QByteArray {}, QByteArray {}).isEmpty());
}
