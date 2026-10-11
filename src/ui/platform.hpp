#pragma once

#include <QByteArray>
#include <QString>

namespace slopkit::ui
{
    [[nodiscard]] QString preferred_platform_spec(const QByteArray& configured, const QByteArray& display);

    void prefer_platform_with_remembered_positions();

}
