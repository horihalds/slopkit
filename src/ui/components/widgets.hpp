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

    // Progress bar filled with the accent colour. `fraction` is clamped to
    // 0..1; when `overlay` is null the percentage is shown.
    void progress_bar(const char*   id,
                      float         fraction,
                      const ImVec2& size    = ImVec2(0.0f, 0.0f),
                      const char*   overlay = nullptr);

    // Draggable divider between two panes. `vertical` is true for a vertical
    // bar that resizes the panes horizontally. `extent` is the combined size of
    // both panes along the drag axis and `span` is the divider's own length.
    // The ratio is mutated while dragging and clamped so neither pane
    // collapses. Returns true when the ratio changed this frame.
    bool splitter(const char* id, bool vertical, float& ratio, float extent, float span);

    // Collapsible themed section. end_group() must be called after every
    // begin_group(), whether or not the group is open.
    bool begin_group(const char* id, const char* title, bool default_open = true);
    void end_group();

    // Compact toolbar button with an optional leading icon text.
    bool toolbar_button(const char* label, const char* icon_text = nullptr);

} // namespace slopkit::ui::widgets
