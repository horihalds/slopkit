#include "ui/app.hpp"

#include <cfloat>
#include <iostream>
#include <utility>

#include <imgui.h>

#include "app/cli.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"

namespace slopkit::ui
{

    int App::run()
    {
        auto window = AppWindow::create();
        if (!window)
        {
            std::cerr << "slopkit: " << window.error() << '\n';
            return 1;
        }
        window_ = std::move(*window);

        theme_ = dark_theme();
        scale_.set_factor(window_->content_scale());
        apply_active_style();
        if (!load_embedded_fonts(kBaseFontSize))
        {
            std::cerr << "slopkit: warning: the embedded fonts could not be loaded\n";
        }
        window_->set_clear_color(theme_.background);

        host_.discover(app::plugin_search_directories());

        while (!window_->should_close())
        {
            window_->poll_events();

            if (window_->take_scale_change())
            {
                scale_.set_factor(window_->content_scale());
                apply_active_style();
            }

            window_->begin_frame();
            draw_ui();
            window_->end_frame();
        }

        // Release GLFW and the GL context before control returns.
        window_.reset();
        return 0;
    }

    void App::apply_active_style()
    {
        ImGui::GetStyle() = scale_.style();
        apply_theme(ImGui::GetStyle(), theme_);
        apply_font_scale(scale_.factor());
    }

    void App::draw_nav_entry(const char* label, Tool tool)
    {
        const ImVec2 size(-FLT_MIN, 0.0f);
        const bool   pressed =
            selected_ == tool ? widgets::primary_button(label, size) : widgets::secondary_button(label, size);
        if (pressed)
        {
            selected_ = tool;
        }
    }

    void App::draw_ui()
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);

        constexpr ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                                                | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings
                                                | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

        ImGui::Begin("slopkit", nullptr, window_flags);

        const float nav_width = 190.0f * scale_.factor();

        ImGui::BeginChild("navigation", ImVec2(nav_width, 0.0f), ImGuiChildFlags_Borders);
        widgets::section_header("Tools");
        draw_nav_entry("Processes", Tool::process_picker);
        draw_nav_entry("Scanner", Tool::scanner);
        draw_nav_entry("Browser", Tool::browser);
        draw_nav_entry("Debugger", Tool::debugger);
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("content", ImVec2(0.0f, 0.0f));
        switch (selected_)
        {
        case Tool::process_picker:
            picker_.draw();
            break;
        case Tool::scanner:
            widgets::section_header("Memory scanner");
            ImGui::TextUnformatted("Planned: search the attached target's memory for values.");
            break;
        case Tool::browser:
            widgets::section_header("Memory browser");
            ImGui::TextUnformatted("Planned: hex view and disassembly of live memory.");
            break;
        case Tool::debugger:
            widgets::section_header("Debugger");
            ImGui::TextUnformatted("Planned: breakpoints, stepping and register inspection.");
            break;
        }
        ImGui::EndChild();

        ImGui::End();
    }

} // namespace slopkit::ui
