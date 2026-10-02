#include "ui/viewports.hpp"

namespace slopkit::ui
{

    void configure_viewports(ImGuiIO& io)
    {
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        // Dialogs are independent top-level windows: keep their taskbar/Alt-Tab
        // entries and never merge them back into the main window when they are
        // dropped onto it.
        io.ConfigViewportsNoAutoMerge   = true;
        io.ConfigViewportsNoTaskBarIcon = false;
    }

    bool viewports_active() noexcept
    {
        const ImGuiIO& io = ImGui::GetIO();
        return (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0
            && (io.BackendFlags & ImGuiBackendFlags_PlatformHasViewports) != 0
            && (io.BackendFlags & ImGuiBackendFlags_RendererHasViewports) != 0;
    }

} // namespace slopkit::ui
