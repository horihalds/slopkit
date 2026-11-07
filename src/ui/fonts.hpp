#pragma once

#include <QFont>

namespace slopkit::ui
{
    inline constexpr int kBaseFontSize = 16;

    [[nodiscard]] bool register_embedded_fonts();

    [[nodiscard]] QFont ui_font();

    [[nodiscard]] QFont mono_font();

}
