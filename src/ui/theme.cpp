#include "ui/theme.hpp"

namespace slopkit::ui
{

    namespace
    {
        constexpr ImVec4 rgba(float red, float green, float blue, float alpha = 1.0f)
        {
            return ImVec4(red, green, blue, alpha);
        }

        Theme g_active = dark_theme();

        void assign_colors(ImGuiStyle& style, const Theme& theme)
        {
            style.Colors[ImGuiCol_Text]                 = theme.text;
            style.Colors[ImGuiCol_TextDisabled]         = theme.text_muted;
            style.Colors[ImGuiCol_WindowBg]             = theme.background;
            style.Colors[ImGuiCol_ChildBg]              = theme.background;
            style.Colors[ImGuiCol_PopupBg]              = theme.surface;
            style.Colors[ImGuiCol_Border]               = theme.border;
            style.Colors[ImGuiCol_BorderShadow]         = rgba(0.0f, 0.0f, 0.0f, 0.0f);
            style.Colors[ImGuiCol_FrameBg]              = theme.surface;
            style.Colors[ImGuiCol_FrameBgHovered]       = theme.surface_hover;
            style.Colors[ImGuiCol_FrameBgActive]        = theme.surface_hover;
            style.Colors[ImGuiCol_TitleBg]              = theme.background;
            style.Colors[ImGuiCol_TitleBgActive]        = theme.surface;
            style.Colors[ImGuiCol_TitleBgCollapsed]     = theme.background;
            style.Colors[ImGuiCol_MenuBarBg]            = theme.surface;
            style.Colors[ImGuiCol_ScrollbarBg]          = theme.background;
            style.Colors[ImGuiCol_ScrollbarGrab]        = theme.surface_hover;
            style.Colors[ImGuiCol_ScrollbarGrabHovered] = theme.border;
            style.Colors[ImGuiCol_ScrollbarGrabActive]  = theme.accent;
            style.Colors[ImGuiCol_CheckMark]            = theme.accent;
            style.Colors[ImGuiCol_SliderGrab]           = theme.accent;
            style.Colors[ImGuiCol_SliderGrabActive]     = theme.accent_active;
            style.Colors[ImGuiCol_PlotHistogram]        = theme.accent;
            style.Colors[ImGuiCol_PlotHistogramHovered] = theme.accent_hover;
            style.Colors[ImGuiCol_Button]               = theme.surface;
            style.Colors[ImGuiCol_ButtonHovered]        = theme.surface_hover;
            style.Colors[ImGuiCol_ButtonActive]         = theme.accent_active;
            style.Colors[ImGuiCol_Header]               = theme.surface_hover;
            style.Colors[ImGuiCol_HeaderHovered]        = theme.accent_hover;
            style.Colors[ImGuiCol_HeaderActive]         = theme.accent;
            style.Colors[ImGuiCol_Separator]            = theme.border;
            style.Colors[ImGuiCol_SeparatorHovered]     = theme.accent_hover;
            style.Colors[ImGuiCol_SeparatorActive]      = theme.accent;
            style.Colors[ImGuiCol_ResizeGrip]           = theme.surface_hover;
            style.Colors[ImGuiCol_ResizeGripHovered]    = theme.accent_hover;
            style.Colors[ImGuiCol_ResizeGripActive]     = theme.accent;
            style.Colors[ImGuiCol_Tab]                  = theme.surface;
            style.Colors[ImGuiCol_TabHovered]           = theme.surface_hover;
            style.Colors[ImGuiCol_TabSelected]          = theme.accent;
            style.Colors[ImGuiCol_TabDimmed]            = theme.background;
            style.Colors[ImGuiCol_TabDimmedSelected]    = theme.surface_hover;
            style.Colors[ImGuiCol_TableHeaderBg]        = theme.surface;
            style.Colors[ImGuiCol_TableBorderStrong]    = theme.border;
            style.Colors[ImGuiCol_TableBorderLight]     = theme.border;
            style.Colors[ImGuiCol_TableRowBg]           = rgba(0.0f, 0.0f, 0.0f, 0.0f);
            style.Colors[ImGuiCol_TableRowBgAlt]        = theme.surface;
            style.Colors[ImGuiCol_TextSelectedBg]       = theme.accent_hover;
            style.Colors[ImGuiCol_NavCursor]            = theme.accent;
            style.Colors[ImGuiCol_ModalWindowDimBg]     = rgba(0.0f, 0.0f, 0.0f, 0.4f);
        }
    } // namespace

    Theme dark_theme()
    {
        Theme theme;
        theme.background    = rgba(0.09f, 0.10f, 0.12f);
        theme.surface       = rgba(0.13f, 0.14f, 0.18f);
        theme.surface_hover = rgba(0.18f, 0.20f, 0.25f);
        theme.text          = rgba(0.92f, 0.93f, 0.95f);
        theme.text_muted    = rgba(0.60f, 0.62f, 0.68f);
        theme.accent        = rgba(0.30f, 0.58f, 0.95f);
        theme.accent_hover  = rgba(0.38f, 0.66f, 0.99f);
        theme.accent_active = rgba(0.24f, 0.49f, 0.84f);
        theme.border        = rgba(0.24f, 0.26f, 0.32f);
        theme.success       = rgba(0.42f, 0.80f, 0.50f);
        theme.warning       = rgba(0.94f, 0.74f, 0.32f);
        theme.error         = rgba(0.92f, 0.44f, 0.44f);
        return theme;
    }

    Theme light_theme()
    {
        Theme theme;
        theme.background    = rgba(0.97f, 0.97f, 0.98f);
        theme.surface       = rgba(1.00f, 1.00f, 1.00f);
        theme.surface_hover = rgba(0.92f, 0.93f, 0.95f);
        theme.text          = rgba(0.12f, 0.13f, 0.16f);
        theme.text_muted    = rgba(0.42f, 0.44f, 0.50f);
        theme.accent        = rgba(0.15f, 0.42f, 0.86f);
        theme.accent_hover  = rgba(0.20f, 0.49f, 0.93f);
        theme.accent_active = rgba(0.11f, 0.34f, 0.72f);
        theme.border        = rgba(0.80f, 0.82f, 0.86f);
        theme.success       = rgba(0.20f, 0.60f, 0.30f);
        theme.warning       = rgba(0.72f, 0.50f, 0.10f);
        theme.error         = rgba(0.78f, 0.22f, 0.22f);
        return theme;
    }

    void apply_theme(ImGuiStyle& style, const Theme& theme)
    {
        g_active = theme;
        assign_colors(style, theme);
    }

    const Theme& active_theme()
    {
        return g_active;
    }

} // namespace slopkit::ui
