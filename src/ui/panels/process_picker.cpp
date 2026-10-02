#include "ui/panels/process_picker.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "ui/components/widgets.hpp"

namespace slopkit::ui::panels
{

    namespace
    {
        std::string lowercase(std::string_view value)
        {
            std::string result(value);
            std::ranges::transform(result,
                                   result.begin(),
                                   [](char character)
                                   {
                                       return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
                                   });
            return result;
        }
    } // namespace

    ProcessPicker::ProcessPicker(process::ProcessAccess& access) : access_(access) {}

    void ProcessPicker::set_status(std::string message, bool is_error)
    {
        status_          = std::move(message);
        status_is_error_ = is_error;
    }

    const process::ProcessInfo* ProcessPicker::selected() const
    {
        if (selected_index_ < 0 || selected_index_ >= static_cast<int>(processes_.size()))
        {
            return nullptr;
        }
        return &processes_[static_cast<std::size_t>(selected_index_)];
    }

    std::string ProcessPicker::chosen_plugin(const process::ProcessInfo& info) const
    {
        if (!selected_plugin_.empty()
            && std::find(info.claimants.begin(), info.claimants.end(), selected_plugin_) != info.claimants.end())
        {
            return selected_plugin_;
        }
        return info.plugin_id;
    }

    void ProcessPicker::refresh()
    {
        const auto*              previous     = selected();
        const process::ProcessId previous_pid = previous != nullptr ? previous->pid : 0;

        auto listed = access_.list_processes();
        if (!listed)
        {
            set_status(std::string("could not list processes: ") + std::string(process::describe(listed.error())),
                       true);
            return;
        }
        processes_ = std::move(*listed);

        plugin_ids_.clear();
        for (const auto& process : processes_)
        {
            for (const auto& claimant : process.claimants)
            {
                if (std::find(plugin_ids_.begin(), plugin_ids_.end(), claimant) == plugin_ids_.end())
                {
                    plugin_ids_.push_back(claimant);
                }
            }
        }
        std::ranges::sort(plugin_ids_);

        // Keep the selection on the same pid across refreshes.
        selected_index_ = -1;
        if (previous_pid != 0)
        {
            for (std::size_t i = 0; i < processes_.size(); ++i)
            {
                if (processes_[i].pid == previous_pid)
                {
                    selected_index_ = static_cast<int>(i);
                    break;
                }
            }
        }
        probe_needed_ = true;
    }

    void ProcessPicker::maybe_auto_refresh()
    {
        if (!auto_refresh_)
        {
            return;
        }

        const double now = ImGui::GetTime();
        if (now >= next_refresh_time_)
        {
            refresh();
            next_refresh_time_ = now + 2.0;
        }
    }

    void ProcessPicker::probe_selection()
    {
        detail_pid_          = -1;
        detail_methods_      = process::AccessMethod::none;
        detail_module_count_ = 0;
        detail_thread_count_ = 0;
        detail_error_.clear();

        const auto* info = selected();
        if (info == nullptr)
        {
            return;
        }
        detail_pid_ = static_cast<int>(info->pid);

        auto probe = access_.attach(info->pid, chosen_plugin(*info));
        if (!probe)
        {
            detail_error_ = std::string("cannot inspect: ") + std::string(process::describe(probe.error()));
            return;
        }

        detail_methods_ = probe->advertised_methods();

        if (const auto modules = probe->modules())
        {
            detail_module_count_ = modules->size();
        }
        else
        {
            detail_error_ = std::string("modules unavailable: ") + std::string(process::describe(modules.error()));
        }
        if (const auto threads = probe->threads())
        {
            detail_thread_count_ = threads->size();
        }
    }

    void ProcessPicker::attach_selected()
    {
        const auto* info = selected();
        if (info == nullptr)
        {
            set_status("Select a process first.", true);
            return;
        }

        session_      = process::Session {};
        auto attached = access_.attach(info->pid, chosen_plugin(*info));
        if (!attached)
        {
            set_status(std::string("attach failed: ") + std::string(process::describe(attached.error())), true);
            return;
        }

        session_ = std::move(*attached);
        set_status("Attached to " + std::to_string(info->pid) + " via " + std::string(session_.plugin_id())
                       + "; methods: " + process::describe(session_.advertised_methods()),
                   false);
    }

    void ProcessPicker::detach()
    {
        if (!session_)
        {
            set_status("Not attached.", true);
            return;
        }
        session_ = process::Session {};
        set_status("Detached.", false);
    }

    void ProcessPicker::draw()
    {
        maybe_auto_refresh();

        if (widgets::primary_button("Refresh"))
        {
            refresh();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Auto-refresh", &auto_refresh_);

        const float toolbar_width = ImGui::GetContentRegionAvail().x;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(toolbar_width * 0.34f);
        ImGui::InputTextWithHint("##process_search", "Filter by name, PID or path", &search_);

        ImGui::SameLine();
        ImGui::SetNextItemWidth(toolbar_width * 0.22f);
        const char* filter_preview = plugin_filter_.empty() ? "All plugins" : plugin_filter_.c_str();
        if (ImGui::BeginCombo("##plugin_filter", filter_preview))
        {
            if (ImGui::Selectable("All plugins", plugin_filter_.empty()))
            {
                plugin_filter_.clear();
            }
            for (const auto& id : plugin_ids_)
            {
                if (ImGui::Selectable(id.c_str(), plugin_filter_ == id))
                {
                    plugin_filter_ = id;
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();

        const ImVec2 region      = ImGui::GetContentRegionAvail();
        const float  body_height = region.y > 0.0f ? region.y - ImGui::GetFrameHeightWithSpacing() : 0.0f;
        const float  list_width  = region.x * 0.62f;

        ImGui::BeginChild("process_list", ImVec2(list_width, body_height), ImGuiChildFlags_Borders);

        if (ImGui::BeginTable("processes",
                              4,
                              ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable
                                  | ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Plugin", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Executable", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            // Apply the search and plugin filters.
            visible_.clear();
            const std::string needle = lowercase(search_);
            for (int index = 0; index < static_cast<int>(processes_.size()); ++index)
            {
                const auto& process = processes_[static_cast<std::size_t>(index)];
                if (!plugin_filter_.empty()
                    && std::find(process.claimants.begin(), process.claimants.end(), plugin_filter_)
                           == process.claimants.end())
                {
                    continue;
                }
                if (!needle.empty())
                {
                    const auto pid_text = std::to_string(process.pid);
                    const bool matches  = lowercase(process.name).find(needle) != std::string::npos
                                       || lowercase(process.exe_path).find(needle) != std::string::npos
                                       || pid_text.find(needle) != std::string::npos;
                    if (!matches)
                    {
                        continue;
                    }
                }
                visible_.push_back(index);
            }

            // Apply the table sort spec.
            int  sort_column = 0;
            bool ascending   = true;
            if (const ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
                specs != nullptr && specs->SpecsCount > 0)
            {
                sort_column = specs->Specs[0].ColumnIndex;
                ascending   = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            }
            std::ranges::sort(visible_,
                              [&](int lhs, int rhs)
                              {
                                  const auto& left  = processes_[static_cast<std::size_t>(lhs)];
                                  const auto& right = processes_[static_cast<std::size_t>(rhs)];
                                  int         order = 0;
                                  switch (sort_column)
                                  {
                                  case 1:
                                      order = left.name.compare(right.name);
                                      break;
                                  case 2:
                                      order = left.plugin_id.compare(right.plugin_id);
                                      break;
                                  case 3:
                                      order = left.exe_path.compare(right.exe_path);
                                      break;
                                  default:
                                      order = left.pid < right.pid ? -1 : (left.pid > right.pid ? 1 : 0);
                                      break;
                                  }
                                  if (order == 0)
                                  {
                                      order = left.pid < right.pid ? -1 : (left.pid > right.pid ? 1 : 0);
                                  }
                                  return ascending ? order < 0 : order > 0;
                              });

            for (const int index : visible_)
            {
                const auto& process = processes_[static_cast<std::size_t>(index)];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(index);
                const auto pid_text = std::to_string(process.pid);
                if (ImGui::Selectable(pid_text.c_str(), selected_index_ == index, ImGuiSelectableFlags_SpanAllColumns))
                {
                    if (selected_index_ != index)
                    {
                        selected_index_ = index;
                        selected_plugin_.clear();
                        probe_needed_ = true;
                    }
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(process.name.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(process.plugin_id.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(process.exe_path.c_str());
                ImGui::PopID();
            }

            ImGui::EndTable();
        }

        ImGui::EndChild();

        // The selection may have changed while drawing the list; refresh the
        // detail data before the detail pane renders.
        if (probe_needed_)
        {
            probe_selection();
            probe_needed_ = false;
        }

        ImGui::SameLine();
        ImGui::BeginChild("process_detail", ImVec2(0.0f, body_height), ImGuiChildFlags_Borders);

        widgets::section_header("Details");
        if (const auto* info = selected(); info == nullptr)
        {
            ImGui::TextUnformatted("Select a process to see its details.");
        }
        else
        {
            ImGui::Text("PID: %u", info->pid);
            ImGui::Text("Name: %s", info->name.c_str());
            ImGui::TextWrapped("Executable: %s", info->exe_path.empty() ? "(unknown)" : info->exe_path.c_str());
            ImGui::Text("Default plugin: %s", info->plugin_id.c_str());

            if (info->claimants.size() > 1)
            {
                ImGui::Spacing();
                ImGui::TextUnformatted("Claimed by:");
                for (const auto& claimant : info->claimants)
                {
                    const bool        is_default = claimant == info->plugin_id;
                    const std::string label      = claimant + (is_default ? " (default)" : "");
                    if (ImGui::RadioButton(label.c_str(), chosen_plugin(*info) == claimant))
                    {
                        selected_plugin_ = claimant;
                        probe_needed_    = true;
                    }
                }
            }

            ImGui::Spacing();
            ImGui::Text("Access methods: %s", process::describe(detail_methods_).c_str());
            ImGui::Text("Modules: %zu", detail_module_count_);
            ImGui::Text("Threads: %zu", detail_thread_count_);

            if (!detail_error_.empty())
            {
                widgets::status_text(widgets::StatusKind::warning, detail_error_.c_str());
            }

            ImGui::Spacing();
            if (session_ && session_.pid() == info->pid)
            {
                if (widgets::secondary_button("Detach"))
                {
                    detach();
                }
                ImGui::SameLine();
                widgets::status_text(widgets::StatusKind::success, "attached");
            }
            else if (widgets::primary_button("Attach"))
            {
                attach_selected();
            }
        }

        ImGui::EndChild();

        ImGui::Separator();
        if (!status_.empty())
        {
            widgets::status_text(status_is_error_ ? widgets::StatusKind::error : widgets::StatusKind::info,
                                 status_.c_str());
        }
        else
        {
            ImGui::TextDisabled("%zu process(es)", processes_.size());
        }
    }

} // namespace slopkit::ui::panels
