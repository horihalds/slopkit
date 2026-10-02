#include "ui/components/widgets.hpp"

#include <algorithm>
#include <string>

#include "ui/theme.hpp"

namespace slopkit::ui::widgets
{

    namespace
    {
        // Divider thickness as a fraction of the font size, so it follows the
        // DPI scale without a raw pixel literal.
        constexpr float kSplitterThicknessEm = 0.4f;

        // Minimum pane size in font-size units, used to keep both panes
        // usable when the divider is dragged to an extreme.
        constexpr float kMinPaneEm = 3.0f;

        float splitter_thickness()
        {
            return ImGui::GetFontSize() * kSplitterThicknessEm;
        }
    } // namespace

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

    void progress_bar(const char* id, float fraction, const ImVec2& size, const char* overlay)
    {
        ImGui::PushID(id);
        ImGui::ProgressBar(std::clamp(fraction, 0.0f, 1.0f), size, overlay);
        ImGui::PopID();
    }

    bool splitter(const char* id, bool vertical, float& ratio, float extent, float span)
    {
        const float  thickness = splitter_thickness();
        const ImVec2 item_size = vertical ? ImVec2(thickness, span) : ImVec2(span, thickness);

        ImGui::PushID(id);
        ImGui::InvisibleButton("##splitter", ImVec2(std::max(item_size.x, 1.0f), std::max(item_size.y, 1.0f)));

        const bool hovered = ImGui::IsItemHovered();
        const bool active  = ImGui::IsItemActive();
        if (hovered || active)
        {
            ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
        }

        const Theme& theme = active_theme();
        const ImVec4 color = active ? theme.accent : (hovered ? theme.accent_hover : theme.border);
        const ImVec2 min   = ImGui::GetItemRectMin();
        const ImVec2 max   = ImGui::GetItemRectMax();
        ImDrawList*  draw  = ImGui::GetWindowDrawList();
        if (vertical)
        {
            const float x = (min.x + max.x) * 0.5f;
            draw->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), ImGui::GetColorU32(color), 1.0f);
        }
        else
        {
            const float y = (min.y + max.y) * 0.5f;
            draw->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), ImGui::GetColorU32(color), 1.0f);
        }

        bool changed = false;
        if (active && extent > 1.0f)
        {
            const float delta = vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
            if (delta != 0.0f)
            {
                const float min_ratio = std::min(0.5f, (ImGui::GetFontSize() * kMinPaneEm) / extent);
                ratio                 = std::clamp(ratio + delta / extent, min_ratio, 1.0f - min_ratio);
                changed               = true;
            }
        }

        ImGui::PopID();
        return changed;
    }

    bool begin_group(const char* id, const char* title, bool default_open)
    {
        const Theme& theme = active_theme();
        ImGui::PushID(id);
        ImGui::PushStyleColor(ImGuiCol_Header, theme.surface);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, theme.surface_hover);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, theme.accent_hover);
        const ImGuiTreeNodeFlags flags = default_open ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
        return ImGui::CollapsingHeader(title, flags);
    }

    void end_group()
    {
        ImGui::PopStyleColor(3);
        ImGui::PopID();
    }

    bool toolbar_button(const char* label, const char* icon_text)
    {
        if (icon_text == nullptr || icon_text[0] == '\0')
        {
            return secondary_button(label);
        }
        const std::string text = std::string(icon_text) + "  " + label;
        return secondary_button(text.c_str());
    }

} // namespace slopkit::ui::widgets
