#include "ui/dialogs/process_list.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "platform/linux/desktop_entry.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"

namespace slopkit::ui::dialogs
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

    ProcessList::ProcessList(process::AccessWorker& worker, process::AttachedTarget& target)
        : worker_(worker), target_(target)
    {
    }

    void ProcessList::set_status(std::string message, bool is_error)
    {
        status_          = std::move(message);
        status_is_error_ = is_error;
    }

    const process::ProcessInfo* ProcessList::selected() const
    {
        if (selected_index_ < 0 || selected_index_ >= static_cast<int>(processes_.size()))
        {
            return nullptr;
        }
        return &processes_[static_cast<std::size_t>(selected_index_)];
    }

    std::string ProcessList::chosen_plugin(const process::ProcessInfo& info) const
    {
        if (!selected_plugin_.empty()
            && std::find(info.claimants.begin(), info.claimants.end(), selected_plugin_) != info.claimants.end())
        {
            return selected_plugin_;
        }
        return info.plugin_id;
    }

    bool ProcessList::is_application(const process::ProcessInfo& info) const
    {
        return platform::is_desktop_application(info.exe_path, application_executables_);
    }

    void ProcessList::refresh()
    {
        if (list_pending_.has_value())
        {
            return; // A list job is already in flight.
        }

        const auto*              previous     = selected();
        const process::ProcessId previous_pid = previous != nullptr ? previous->pid : 0;

        const process::JobId job_id = worker_.next_job_id();
        list_pending_               = job_id;

        const bool submitted = worker_.submit_list(
            job_id,
            [this, previous_pid, job_id](process::JobResult&& result)
            {
                if (list_pending_ != job_id)
                {
                    return; // Superseded or shut down.
                }
                list_pending_.reset();

                auto& listed = std::get<process::ListResult>(result);
                if (listed.error)
                {
                    set_status(std::string("could not list processes: ")
                                   + std::string(process::describe(*listed.error)),
                               true);
                    return;
                }
                processes_ = std::move(listed.processes);

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
            });
        if (!submitted)
        {
            list_pending_.reset();
        }
    }

    void ProcessList::maybe_auto_refresh()
    {
        if (!auto_refresh_)
        {
            return;
        }

        const double now = ImGui::GetTime();
        if (now >= next_refresh_time_)
        {
            refresh(); // No-ops while a list job is already pending.
            next_refresh_time_ = now + 2.0;
        }
    }

    void ProcessList::request_application_index()
    {
        if (index_built_ || index_pending_.has_value())
        {
            return;
        }

        const process::JobId job_id = worker_.next_job_id();
        index_pending_              = job_id;

        const bool submitted =
            worker_.submit_application_index(job_id,
                                             [this, job_id](process::JobResult&& result)
                                             {
                                                 if (index_pending_ != job_id)
                                                 {
                                                     return;
                                                 }
                                                 index_pending_.reset();
                                                 application_executables_ =
                                                     std::move(std::get<process::AppIndexResult>(result).executables);
                                                 index_built_ = true;
                                             });
        if (!submitted)
        {
            index_pending_.reset();
        }
    }

    void ProcessList::probe_selection()
    {
        const auto* info = selected();
        if (info == nullptr)
        {
            detail_pid_          = -1;
            detail_methods_      = process::AccessMethod::none;
            detail_module_count_ = 0;
            detail_thread_count_ = 0;
            detail_error_.clear();
            return;
        }
        if (probe_pending_.has_value() && probe_pending_pid_ == static_cast<int>(info->pid))
        {
            return; // Already inspecting this process.
        }

        const process::ProcessId pid    = info->pid;
        const std::string        plugin = chosen_plugin(*info);

        // Clear the cached detail and show the placeholder until it lands.
        detail_pid_          = static_cast<int>(pid);
        detail_methods_      = process::AccessMethod::none;
        detail_module_count_ = 0;
        detail_thread_count_ = 0;
        detail_error_.clear();

        const process::JobId job_id = worker_.next_job_id();
        probe_pending_              = job_id;
        probe_pending_pid_          = static_cast<int>(pid);

        const bool submitted = worker_.submit_probe(
            job_id,
            pid,
            plugin,
            [this, pid, job_id](process::JobResult&& result)
            {
                if (probe_pending_ != job_id)
                {
                    return; // Superseded by a newer probe.
                }
                probe_pending_.reset();
                probe_pending_pid_ = -1;

                // Drop a result whose selection changed while it was in flight.
                const auto* current = selected();
                if (detail_pid_ != static_cast<int>(pid) || current == nullptr || current->pid != pid)
                {
                    return;
                }

                auto& probe = std::get<process::ProbeResult>(result);
                if (probe.error)
                {
                    detail_error_ = std::string("cannot inspect: ") + std::string(process::describe(*probe.error));
                    return;
                }
                detail_methods_      = probe.method;
                detail_module_count_ = probe.modules;
                detail_thread_count_ = probe.threads;
                if (probe.modules_error)
                {
                    detail_error_ =
                        std::string("modules unavailable: ") + std::string(process::describe(*probe.modules_error));
                }
            });
        if (!submitted)
        {
            probe_pending_.reset();
            probe_pending_pid_ = -1;
        }
    }

    void ProcessList::attach_selected()
    {
        const auto* info = selected();
        if (info == nullptr)
        {
            set_status("Select a process first.", true);
            return;
        }
        if (attach_pending_.has_value())
        {
            return;
        }

        const process::ProcessId pid    = info->pid;
        const std::string        name   = info->name;
        const std::string        plugin = chosen_plugin(*info);

        const process::JobId job_id = worker_.next_job_id();
        attach_pending_             = job_id;

        const bool submitted = worker_.submit_attach_app(
            job_id,
            pid,
            plugin,
            [this, pid, name, job_id](process::JobResult&& result)
            {
                if (attach_pending_ != job_id)
                {
                    return; // Superseded or shut down.
                }
                attach_pending_.reset();

                const auto& attached = std::get<process::AttachResult>(result);
                if (attached.error)
                {
                    target_.clear();
                    set_status(std::string("attach failed: ") + std::string(process::describe(*attached.error)), true);
                    return;
                }

                target_.clear();
                target_.pid          = pid;
                target_.name         = name;
                target_.plugin_id    = attached.info->plugin_id;
                target_.method       = attached.info->method;
                target_.session_live = true;
                set_status(target_.label() + "; methods: " + process::describe(target_.method), false);
            });
        if (!submitted)
        {
            attach_pending_.reset();
            set_status("Attach unavailable.", true);
        }
    }

    void ProcessList::detach()
    {
        if (!target_.valid())
        {
            set_status("Not attached.", true);
            return;
        }
        if (detach_pending_.has_value())
        {
            return;
        }

        const process::JobId job_id = worker_.next_job_id();
        detach_pending_             = job_id;

        const bool submitted = worker_.submit_detach(job_id,
                                                     [this, job_id](process::JobResult&&)
                                                     {
                                                         if (detach_pending_ != job_id)
                                                         {
                                                             return;
                                                         }
                                                         detach_pending_.reset();
                                                         target_.clear();
                                                         set_status("Detached.", false);
                                                     });
        if (!submitted)
        {
            detach_pending_.reset();
            set_status("Detach unavailable.", true);
        }
    }

    void ProcessList::draw(bool& open)
    {
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 52.0f, ImGui::GetFontSize() * 34.0f),
                                 ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Process List", &open))
        {
            ImGui::End();
            return;
        }

        maybe_auto_refresh();
        request_application_index();

        if (!status_.empty())
        {
            widgets::status_text(status_is_error_ ? widgets::StatusKind::error : widgets::StatusKind::info,
                                 status_.c_str());
        }
        else
        {
            ImGui::TextDisabled("%zu process(es)", processes_.size());
        }

        const bool refreshing = list_pending_.has_value();
        ImGui::BeginDisabled(refreshing);
        if (widgets::primary_button(refreshing ? "Refreshing..." : "Refresh"))
        {
            refresh();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("Auto-refresh", &auto_refresh_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        ImGui::InputTextWithHint("##process_search", "Filter by name, PID or path", &search_);

        if (!plugin_ids_.empty())
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            const char* preview = plugin_filter_.empty() ? "All plugins" : plugin_filter_.c_str();
            if (ImGui::BeginCombo("##plugin_filter", preview))
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
        }

        ImGui::Spacing();

        if (ImGui::BeginTabBar("##process_tabs"))
        {
            if (ImGui::BeginTabItem("Applications"))
            {
                draw_body(true);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Processes"))
            {
                draw_body(false);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::End();
    }

    void ProcessList::draw_body(bool applications_only)
    {
        const ImVec2 region      = ImGui::GetContentRegionAvail();
        const float  body_height = std::max(region.y, 1.0f);
        const float  list_width  = std::max(region.x * 0.62f, 1.0f);

        ImGui::BeginChild("process_list", ImVec2(list_width, body_height), ImGuiChildFlags_Borders);

        if (applications_only && !index_built_)
        {
            ImGui::TextDisabled("Indexing applications...");
        }
        else if (ImGui::BeginTable("processes",
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

            // Apply the tab, search and plugin filters.
            visible_.clear();
            const std::string needle = lowercase(search_);
            for (int index = 0; index < static_cast<int>(processes_.size()); ++index)
            {
                const auto& process = processes_[static_cast<std::size_t>(index)];
                if (is_application(process) != applications_only)
                {
                    continue;
                }
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
                {
                    ui::ScopedMonoFont mono;
                    if (ImGui::Selectable(
                            pid_text.c_str(), selected_index_ == index, ImGuiSelectableFlags_SpanAllColumns))
                    {
                        if (selected_index_ != index)
                        {
                            selected_index_ = index;
                            selected_plugin_.clear();
                            probe_needed_ = true;
                        }
                    }
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(process.name.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(process.plugin_id.c_str());
                ImGui::TableSetColumnIndex(3);
                {
                    ui::ScopedMonoFont mono;
                    ImGui::TextUnformatted(process.exe_path.c_str());
                }
                ImGui::PopID();
            }

            if (visible_.empty())
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled(applications_only ? "No applications found." : "No processes found.");
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
        if (const auto* info = selected(); info != nullptr)
        {
            draw_detail(*info);
        }
        else
        {
            ImGui::TextUnformatted("Select a process to see its details.");
        }
        ImGui::EndChild();
    }

    void ProcessList::draw_detail(const process::ProcessInfo& info)
    {
        widgets::section_header("Details");

        {
            ui::ScopedMonoFont mono;
            ImGui::Text("PID: %u", info.pid);
        }
        ImGui::Text("Name: %s", info.name.c_str());
        {
            ui::ScopedMonoFont mono;
            ImGui::TextWrapped("Executable: %s", info.exe_path.empty() ? "(unknown)" : info.exe_path.c_str());
        }
        ImGui::Text("Default plugin: %s", info.plugin_id.c_str());

        if (info.claimants.size() > 1)
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Claimed by:");
            for (const auto& claimant : info.claimants)
            {
                const bool        is_default = claimant == info.plugin_id;
                const std::string label      = claimant + (is_default ? " (default)" : "");
                if (ImGui::RadioButton(label.c_str(), chosen_plugin(info) == claimant))
                {
                    selected_plugin_ = claimant;
                    probe_needed_    = true;
                }
            }
        }

        ImGui::Spacing();
        if (probe_pending_.has_value() && probe_pending_pid_ == static_cast<int>(info.pid))
        {
            ImGui::TextDisabled("Inspecting...");
        }
        else
        {
            ui::ScopedMonoFont mono;
            ImGui::Text("Access methods: %s", process::describe(detail_methods_).c_str());
            ImGui::Text("Modules: %zu", detail_module_count_);
            ImGui::Text("Threads: %zu", detail_thread_count_);
        }

        if (!detail_error_.empty())
        {
            widgets::status_text(widgets::StatusKind::warning, detail_error_.c_str());
        }

        ImGui::Spacing();
        if (target_.valid() && target_.pid == info.pid)
        {
            const bool pending = detach_pending_.has_value();
            ImGui::BeginDisabled(pending);
            if (widgets::secondary_button(pending ? "Detaching..." : "Detach"))
            {
                detach();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            widgets::status_text(widgets::StatusKind::success, "attached");
        }
        else
        {
            const bool pending = attach_pending_.has_value();
            ImGui::BeginDisabled(pending);
            if (widgets::primary_button(pending ? "Attaching..." : "Attach"))
            {
                attach_selected();
            }
            ImGui::EndDisabled();
        }
    }

} // namespace slopkit::ui::dialogs
