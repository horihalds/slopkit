#include "ui/dialogs/settings.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <imgui.h>

#include "core/version.hpp"
#include "ui/components/widgets.hpp"

namespace slopkit::ui::dialogs
{

    namespace
    {
        constexpr const char* kCategories[] = {"Appearance", "Scanning", "Plugins", "About"};
        constexpr int         kAboutIndex   = 3;

        bool parse_number(const char* text, std::uint64_t& out)
        {
            const std::string_view view(text);
            if (view.empty())
            {
                return false;
            }
            const auto [end, error] = std::from_chars(view.data(), view.data() + view.size(), out);
            return error == std::errc {} && end == view.data() + view.size();
        }
    } // namespace

    Settings::Settings(plugin::PluginHost& host, scan::ScanEngine& engine) : host_(host), engine_(engine) {}

    void Settings::select_about()
    {
        category_ = kAboutIndex;
    }

    void Settings::set_status(std::string message, bool is_error)
    {
        status_          = std::move(message);
        status_is_error_ = is_error;
    }

    Settings::Result Settings::draw(bool& open, bool dark_theme)
    {
        Result result;

        ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 46.0f, ImGui::GetFontSize() * 24.0f),
                                 ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Settings", &open))
        {
            ImGui::End();
            return result;
        }

        const float list_width  = ImGui::GetFontSize() * 11.0f;
        const float body_height = ImGui::GetContentRegionAvail().y;

        ImGui::BeginChild("settings_categories", ImVec2(list_width, body_height), ImGuiChildFlags_Borders);
        for (int index = 0; index < IM_ARRAYSIZE(kCategories); ++index)
        {
            if (ImGui::Selectable(kCategories[index], category_ == index))
            {
                category_ = index;
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("settings_body", ImVec2(0.0f, body_height), ImGuiChildFlags_Borders);
        switch (category_)
        {
        case 0:
            draw_appearance(result, dark_theme);
            break;
        case 1:
            draw_scanning(result);
            break;
        case 2:
            draw_plugins();
            break;
        default:
            draw_about();
            break;
        }
        ImGui::EndChild();

        if (!status_.empty())
        {
            widgets::status_text(status_is_error_ ? widgets::StatusKind::error : widgets::StatusKind::info,
                                 status_.c_str());
        }

        ImGui::End();
        return result;
    }

    void Settings::draw_appearance(Result& result, bool dark_theme)
    {
        widgets::section_header("Appearance");

        ImGui::TextUnformatted("Theme");
        const int theme = dark_theme ? 0 : 1;
        if (ImGui::RadioButton("Dark", theme == 0))
        {
            result.dark_theme = true;
        }
        if (ImGui::RadioButton("Light", theme == 1))
        {
            result.dark_theme = false;
        }

        ImGui::Spacing();
        ImGui::TextDisabled("The theme applies live and is not persisted between runs.");
    }

    void Settings::draw_scanning(Result& result)
    {
        widgets::section_header("Scanning");

        ImGui::TextUnformatted("Default fast-scan alignment (bytes)");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        ImGui::InputText("##default_alignment", alignment_.data(), alignment_.size());
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Used when the scan controls' alignment field is left blank");
        }

        ImGui::TextUnformatted("Stored result cap");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        ImGui::InputText("##result_cap", result_cap_.data(), result_cap_.size());

        ImGui::Spacing();
        if (widgets::primary_button("Apply"))
        {
            apply_scanning(result);
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Defaults apply to this session; nothing is persisted between runs.");
    }

    void Settings::apply_scanning(Result& result)
    {
        std::uint64_t alignment = 0;
        if (!parse_number(alignment_.data(), alignment) || alignment == 0)
        {
            set_status("Alignment must be a positive number.", true);
            return;
        }
        std::uint64_t cap = 0;
        if (!parse_number(result_cap_.data(), cap) || cap == 0)
        {
            set_status("The result cap must be a positive number.", true);
            return;
        }

        engine_.set_max_stored_hits(static_cast<std::size_t>(cap));
        result.default_alignment = alignment;
        set_status("Scanning defaults applied.", false);
    }

    void Settings::draw_plugins()
    {
        widgets::section_header("Plugins");

        const auto plugins = host_.plugins();
        if (plugins.empty())
        {
            widgets::status_text(widgets::StatusKind::warning, "No plugins were loaded.");
        }
        for (const auto& plugin : plugins)
        {
            ImGui::Text("%s v%s (precedence %d)",
                        std::string(plugin->id()).c_str(),
                        std::string(plugin->version()).c_str(),
                        plugin->precedence());
            ImGui::TextDisabled("  %s", std::string(plugin->name()).c_str());
            ImGui::TextDisabled("  %s", plugin->path().string().c_str());
            if (!plugin->description().empty())
            {
                ImGui::TextDisabled("  %s", std::string(plugin->description()).c_str());
            }
            ImGui::Spacing();
        }

        if (!host_.diagnostics().empty())
        {
            widgets::section_header("Diagnostics");
            for (const auto& diagnostic : host_.diagnostics())
            {
                widgets::status_text(widgets::StatusKind::warning,
                                     (diagnostic.path.string() + ": " + diagnostic.message).c_str());
            }
        }
    }

    void Settings::draw_about()
    {
        widgets::section_header("About");

        ImGui::Text("slopkit %s", std::string(slopkit::version()).c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("A plugin-first memory scanner and debugger front end. The host never touches another "
                           "process directly: all access is provided by plugins.");
        ImGui::Spacing();
        ImGui::TextDisabled("Theme: dark/light, applied live from the Appearance category.");
        ImGui::TextDisabled("Plugins loaded: %zu", host_.plugins().size());
    }

} // namespace slopkit::ui::dialogs
