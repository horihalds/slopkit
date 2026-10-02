#include <catch2/catch.hpp>

#include <imgui.h>

#include "ui/viewports.hpp"

namespace
{
    // The viewport configuration works on ImGui types only, so a bare context
    // without a window or backend is enough.
    struct ContextGuard
    {
        ContextGuard()
        {
            ImGui::CreateContext();
        }

        ~ContextGuard()
        {
            ImGui::DestroyContext();
        }

        ContextGuard(const ContextGuard&)            = delete;
        ContextGuard& operator=(const ContextGuard&) = delete;
    };
} // namespace

TEST_CASE("configure_viewports enables multi-viewport without docking", "[ui]")
{
    ContextGuard guard;

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;

    slopkit::ui::configure_viewports(io);

    CHECK((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0);
    CHECK((io.ConfigFlags & ImGuiConfigFlags_DockingEnable) == 0);
    CHECK(io.ConfigViewportsNoAutoMerge);
    CHECK_FALSE(io.ConfigViewportsNoTaskBarIcon);
}

TEST_CASE("this ImGui build exposes multi-viewport support", "[ui]")
{
    // These symbols only exist on the ImGui docking branch, so this test fails
    // to compile if the pin ever slips back to master.
    CHECK(ImGuiConfigFlags_ViewportsEnable != 0);
    CHECK(ImGuiBackendFlags_PlatformHasViewports != 0);
    CHECK(ImGuiBackendFlags_RendererHasViewports != 0);
}

TEST_CASE("viewports_active requires backend and renderer support", "[ui]")
{
    ContextGuard guard;

    ImGuiIO& io = ImGui::GetIO();
    slopkit::ui::configure_viewports(io);

    // A bare context has no backend, so platform viewports are not available.
    CHECK_FALSE(slopkit::ui::viewports_active());

    io.BackendFlags |= ImGuiBackendFlags_PlatformHasViewports | ImGuiBackendFlags_RendererHasViewports;
    CHECK(slopkit::ui::viewports_active());
}
