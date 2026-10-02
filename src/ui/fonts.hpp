#pragma once

#include <imgui.h>

namespace slopkit::ui
{

    // The base font size in logical units at scale 1.0.
    inline constexpr float kBaseFontSize = 16.0f;

    // Loads the embedded Noto Sans into the current ImGui context's font atlas.
    // The font data is static, so the atlas must not own it. Returns the font or
    // null on failure.
    [[nodiscard]] ImFont* load_embedded_font(float base_size = kBaseFontSize);

    // Applies a DPI scale to fonts. The 1.92+ dynamic font system scales the
    // loaded font at draw time, so no atlas rebuild is needed. Call between
    // frames, never mid-frame.
    void apply_font_scale(float scale);

} // namespace slopkit::ui
