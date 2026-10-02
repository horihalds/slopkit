#pragma once

#include <imgui.h>

namespace slopkit::ui::widgets
{

    enum class StatusKind
    {
        info,
        success,
        warning,
        error,
    };

    // Muted label with a separator, used to title sections inside panels.
    void section_header(const char* label);

    // Filled, accent-coloured button for the primary action.
    bool primary_button(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));

    // Muted, surface-coloured button for secondary actions.
    bool secondary_button(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));

    // Card-like surface. EndChild must be paired with every BeginChild, so
    // end_panel() must always be called after begin_panel().
    bool begin_panel(const char* id, const char* title);
    void end_panel();

    // Wrapped status line coloured by kind.
    void status_text(StatusKind kind, const char* text);

} // namespace slopkit::ui::widgets
