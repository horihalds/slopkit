#pragma once

#include <QFont>

namespace slopkit::ui
{

    // The base font size in logical pixels; Qt scales it with the device pixel
    // ratio.
    inline constexpr int kBaseFontSize = 16;

    // Registers the embedded Noto Sans and Noto Sans Mono with the font
    // database and installs Noto Sans as the application font. Returns false
    // when either face could not be registered, in which case the callers fall
    // back to the system fonts. Requires a QGuiApplication.
    [[nodiscard]] bool register_embedded_fonts();

    // The proportional face (Noto Sans).
    [[nodiscard]] QFont ui_font();

    // The monospace face (Noto Sans Mono); used for addresses and hex dumps.
    [[nodiscard]] QFont mono_font();

} // namespace slopkit::ui
