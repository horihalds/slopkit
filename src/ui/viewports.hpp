#pragma once

#include <imgui.h>

namespace slopkit::ui
{

    // Turns on ImGui multi-viewport so dialogs become real OS windows: own
    // decorations, taskbar/Alt-Tab entries, movable outside the main window and
    // never merged back into it. Kept GLFW-free so it can be tested on a bare
    // ImGui context.
    void configure_viewports(ImGuiIO& io);

    // True when viewports are enabled and the configured backend and renderer
    // both expose platform-window support.
    [[nodiscard]] bool viewports_active() noexcept;

} // namespace slopkit::ui
