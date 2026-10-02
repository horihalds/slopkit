#pragma once

#include <imgui.h>

namespace slopkit::ui
{

    // The base font size in logical units at scale 1.0.
    inline constexpr float kBaseFontSize = 16.0f;

    // Loads the embedded Noto Sans and Noto Sans Mono into the current ImGui
    // context's font atlas and caches them for ui_font()/mono_font(). The font
    // data is static, so the atlas must not own it. Returns false on failure.
    [[nodiscard]] bool load_embedded_fonts(float base_size = kBaseFontSize);

    // The proportional face (Noto Sans); null before load_embedded_fonts().
    [[nodiscard]] ImFont* ui_font();

    // The monospace face (Noto Sans Mono); null before load_embedded_fonts().
    [[nodiscard]] ImFont* mono_font();

    // Pushes the monospace font for the enclosing scope. A no-op when the
    // monospace face is unavailable, so callers degrade to the proportional font
    // without a push/pop imbalance.
    class ScopedMonoFont
    {
    public:
        ScopedMonoFont();
        ~ScopedMonoFont();

        ScopedMonoFont(const ScopedMonoFont&)            = delete;
        ScopedMonoFont& operator=(const ScopedMonoFont&) = delete;

    private:
        bool pushed_ = false;
    };

    // Applies a DPI scale to fonts. The 1.92+ dynamic font system scales the
    // loaded font at draw time, so no atlas rebuild is needed. Call between
    // frames, never mid-frame.
    void apply_font_scale(float scale);

} // namespace slopkit::ui
