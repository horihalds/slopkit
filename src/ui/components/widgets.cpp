#include "ui/components/widgets.hpp"

#include "ui/theme.hpp"

namespace slopkit::ui::widgets
{

    void section_header(const char* label)
    {
        const Theme& theme = active_theme();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.text_muted);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();
    }

    bool primary_button(const char* label, const ImVec2& size)
    {
        const Theme& theme = active_theme();
        ImGui::PushStyleColor(ImGuiCol_Button, theme.accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.accent_hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.accent_active);
        const bool pressed = ImGui::Button(label, size);
        ImGui::PopStyleColor(3);
        return pressed;
    }

    bool secondary_button(const char* label, const ImVec2& size)
    {
        const Theme& theme = active_theme();
        ImGui::PushStyleColor(ImGuiCol_Button, theme.surface);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.surface_hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.surface_hover);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.text);
        const bool pressed = ImGui::Button(label, size);
        ImGui::PopStyleColor(4);
        return pressed;
    }

    bool begin_panel(const char* id, const char* title)
    {
        const Theme& theme = active_theme();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.surface);
        const bool visible = ImGui::BeginChild(id, ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
        if (visible && title != nullptr && title[0] != '\0')
        {
            section_header(title);
        }
        return visible;
    }

    void end_panel()
    {
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void status_text(StatusKind kind, const char* text)
    {
        const Theme& theme = active_theme();

        ImVec4 color = theme.text;
        switch (kind)
        {
        case StatusKind::info:
            color = theme.text_muted;
            break;
        case StatusKind::success:
            color = theme.success;
            break;
        case StatusKind::warning:
            color = theme.warning;
            break;
        case StatusKind::error:
            color = theme.error;
            break;
        }

        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
    }

} // namespace slopkit::ui::widgets
