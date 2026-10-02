#include "ui/app.hpp"

#include <algorithm>
#include <cfloat>
#include <iostream>
#include <utility>
#include <variant>

#include <imgui.h>

#include "app/cli.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"
#include "ui/panels/top_bar.hpp"

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

        while (!window_->should_close() && !should_quit_)
        {
            window_->poll_events();

            if (window_->take_scale_change())
            {
                scale_.set_factor(window_->content_scale());
                apply_active_style();
            }

            window_->begin_frame();

            // Apply finished background jobs before the panels read the model.
            access_worker_.drain();

            if (target_.valid() && !freeze_pending_.has_value())
            {
                auto items = address_table_.freeze_items(ImGui::GetTime());
                if (!items.empty())
                {
                    const process::JobId job_id = access_worker_.next_job_id();
                    freeze_pending_             = job_id;
                    const bool submitted        = access_worker_.submit_freeze(
                        job_id,
                        std::move(items),
                        [this, job_id](process::JobResult&& result)
                        {
                            if (freeze_pending_ != job_id)
                            {
                                return;
                            }
                            freeze_pending_.reset();
                            const auto& frozen = std::get<process::FreezeResult>(result);
                            if (frozen.error)
                            {
                                address_list_.report_freeze_error(process::describe(*frozen.error));
                            }
                        });
                    if (!submitted)
                    {
                        freeze_pending_.reset();
                    }
                }
            }

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

    void App::draw_ui()
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);

        constexpr ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                                                | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings
                                                | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus
                                                | ImGuiWindowFlags_MenuBar;

        ImGui::Begin("slopkit", nullptr, window_flags);

        // Top bar.
        panels::TopBar::Model top_model;
        top_model.process_label               = target_.label();
        top_model.progress                    = scanner_.engine().snapshot().progress;
        top_model.scanning                    = scanner_.engine().is_running();
        const panels::TopBar::Actions actions = top_bar_.draw(top_model);

        if (actions.open_process)
        {
            show_process_list_ = true;
        }
        if (actions.undo_scan)
        {
            scanner_.engine().undo();
        }
        if (actions.open_table)
        {
            address_list_.request_open();
        }
        if (actions.save_table)
        {
            address_list_.request_save();
        }
        if (actions.delete_selected)
        {
            address_list_.delete_selected();
        }
        if (actions.freeze_selected)
        {
            address_list_.toggle_freeze_selected();
        }
        if (actions.settings)
        {
            show_settings_ = true;
        }
        if (actions.add_address)
        {
            show_add_address_ = true;
        }
        if (actions.about)
        {
            settings_.select_about();
            show_settings_ = true;
        }
        if (actions.quit)
        {
            should_quit_ = true;
        }

        // Middle and bottom zones, separated by draggable dividers.
        const ImVec2 spacing  = ImGui::GetStyle().ItemSpacing;
        const float  splitter = ImGui::GetFontSize() * 0.4f;
        const ImVec2 region   = ImGui::GetContentRegionAvail();
        const float  min_pane = ImGui::GetFontSize() * 3.0f;
        float        usable_h = region.y - splitter - 2.0f * spacing.y;
        if (usable_h < min_pane * 2.0f)
        {
            usable_h = std::max(region.y - splitter - 2.0f * spacing.y, 1.0f);
        }
        const float middle_h = usable_h * middle_ratio_;
        const float bottom_h = std::max(usable_h - middle_h, 1.0f);

        ImGui::BeginChild("zone_middle", ImVec2(0.0f, middle_h), ImGuiChildFlags_None);
        {
            const float mid_inner_h = ImGui::GetContentRegionAvail().y;
            const float min_w       = ImGui::GetFontSize() * 6.0f;
            float       usable_w    = ImGui::GetContentRegionAvail().x - splitter - 2.0f * spacing.x;
            if (usable_w < min_w * 2.0f)
            {
                usable_w = std::max(ImGui::GetContentRegionAvail().x - splitter - 2.0f * spacing.x, 1.0f);
            }
            const float left_w  = usable_w * found_ratio_;
            const float right_w = std::max(usable_w - left_w, 1.0f);

            ImGui::BeginChild("zone_found", ImVec2(left_w, 0.0f), ImGuiChildFlags_None);
            found_list_.draw();
            ImGui::EndChild();

            ImGui::SameLine();
            widgets::splitter("split_found", true, found_ratio_, usable_w, mid_inner_h);

            ImGui::SameLine();
            ImGui::BeginChild("zone_scanner", ImVec2(right_w, 0.0f), ImGuiChildFlags_None);
            scanner_.draw();
            ImGui::EndChild();
        }
        ImGui::EndChild();

        widgets::splitter("split_rows", false, middle_ratio_, usable_h, region.x);

        ImGui::BeginChild("zone_address_list", ImVec2(0.0f, bottom_h), ImGuiChildFlags_None);
        address_list_.draw();
        ImGui::EndChild();

        ImGui::End();

        // Requests raised by the panels while they were drawn this frame.
        if (const auto address = scanner_.take_memory_view_request(); address.has_value())
        {
            memory_viewer_.set_address(*address);
            show_memory_viewer_ = true;
        }
        if (scanner_.take_add_address_request())
        {
            show_add_address_ = true;
        }
        if (const auto address = address_list_.take_browse_request(); address.has_value())
        {
            memory_viewer_.set_address(*address);
            show_memory_viewer_ = true;
        }

        if (show_process_list_)
        {
            process_list_.draw(show_process_list_);
        }
        if (show_add_address_)
        {
            add_address_.draw(show_add_address_);
        }
        if (show_memory_viewer_)
        {
            memory_viewer_.draw(show_memory_viewer_);
        }
        if (show_settings_)
        {
            const dialogs::Settings::Result result = settings_.draw(show_settings_, theme_is_dark_);
            if (result.dark_theme.has_value())
            {
                theme_is_dark_ = *result.dark_theme;
                theme_         = theme_is_dark_ ? dark_theme() : light_theme();
                apply_active_style();
            }
            if (result.default_alignment.has_value())
            {
                scanner_.set_default_alignment(*result.default_alignment);
            }
        }
    }

} // namespace slopkit::ui
