#include "ui/panels/top_bar.hpp"

#include <algorithm>
#include <cfloat>
#include <cstdio>

#include <imgui.h>

#include "ui/components/widgets.hpp"

namespace slopkit::ui::panels
{

    TopBar::Actions TopBar::draw(const Model& model)
    {
        Actions actions;

        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Open Process..."))
                {
                    actions.open_process = true;
                }
                if (ImGui::MenuItem("Open Table..."))
                {
                    actions.open_table = true;
                }
                if (ImGui::MenuItem("Save Table..."))
                {
                    actions.save_table = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Quit"))
                {
                    actions.quit = true;
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Edit"))
            {
                if (ImGui::MenuItem("Undo Scan"))
                {
                    actions.undo_scan = true;
                }
                if (ImGui::MenuItem("Add Address Manually..."))
                {
                    actions.add_address = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Settings..."))
                {
                    actions.settings = true;
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Table"))
            {
                if (ImGui::MenuItem("Delete Selected"))
                {
                    actions.delete_selected = true;
                }
                if (ImGui::MenuItem("Freeze Selected"))
                {
                    actions.freeze_selected = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Save"))
                {
                    actions.save_table = true;
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("D3D"))
            {
                ImGui::MenuItem("Placeholder", nullptr, false, false);
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Help"))
            {
                if (ImGui::MenuItem("About slopkit"))
                {
                    actions.about = true;
                }
                ImGui::EndMenu();
            }

            ImGui::EndMenuBar();
        }

        // Toolbar row: the file actions on the left, Settings pinned right.
        if (widgets::toolbar_button("Open Process"))
        {
            actions.open_process = true;
        }
        ImGui::SameLine();
        if (widgets::toolbar_button("Open Table"))
        {
            actions.open_table = true;
        }
        ImGui::SameLine();
        if (widgets::toolbar_button("Save Table"))
        {
            actions.save_table = true;
        }

        const float settings_width = ImGui::CalcTextSize("Settings").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float settings_x     = ImGui::GetContentRegionMax().x - settings_width;
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), settings_x));
        if (widgets::toolbar_button("Settings"))
        {
            actions.settings = true;
        }

        ImGui::Spacing();

        if (model.process_label.empty())
        {
            ImGui::TextDisabled("No Process Selected");
        }
        else
        {
            ImGui::TextUnformatted(model.process_label.c_str());
        }

        char overlay[32];
        std::snprintf(overlay, sizeof(overlay), "%.0f%%", model.progress * 100.0f);
        widgets::progress_bar("##scan_progress", model.progress, ImVec2(-FLT_MIN, 0.0f), overlay);

        return actions;
    }

} // namespace slopkit::ui::panels
