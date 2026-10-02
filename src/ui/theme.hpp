#pragma once

#include <imgui.h>

namespace slopkit::ui
{

    // Semantic colour roles. Every role is defined by both themes, and widget
    // code must read colours from the active theme rather than hard-coding them.
    struct Theme
    {
        ImVec4 background;
        ImVec4 surface;
        ImVec4 surface_hover;
        ImVec4 text;
        ImVec4 text_muted;
        ImVec4 accent;
        ImVec4 accent_hover;
        ImVec4 accent_active;
        ImVec4 border;
        ImVec4 success;
        ImVec4 warning;
        ImVec4 error;
    };

    [[nodiscard]] Theme dark_theme();
    [[nodiscard]] Theme light_theme();

    // Applies a theme to a style and makes it the active theme the component
    // library reads from.
    void apply_theme(ImGuiStyle& style, const Theme& theme);

    [[nodiscard]] const Theme& active_theme();

} // namespace slopkit::ui
