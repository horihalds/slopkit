#include "ui/panels/address_list_panel.hpp"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <format>
#include <string>
#include <utility>

#include <imgui.h>

#include "process/types.hpp"
#include "scan/types.hpp"
#include "scan/value.hpp"
#include "table/serializer.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"

namespace slopkit::ui::panels
{

    AddressListPanel::AddressListPanel(table::AddressTable&     table,
                                       process::AccessWorker&   worker,
                                       process::AttachedTarget& target)
        : table_(table), worker_(worker), target_(target)
    {
    }

    void AddressListPanel::set_status(std::string message, bool is_error)
    {
        status_          = std::move(message);
        status_is_error_ = is_error;
    }

    void AddressListPanel::request_open()
    {
        open_requested_ = true;
    }

    void AddressListPanel::request_save()
    {
        save_requested_ = true;
    }

    void AddressListPanel::report_freeze_error(std::string_view message)
    {
        set_status("Freeze failed: " + std::string(message), true);
    }

    std::optional<std::uint64_t> AddressListPanel::take_browse_request()
    {
        auto request = browse_request_;
        browse_request_.reset();
        return request;
    }

    void AddressListPanel::delete_selected()
    {
        const int index = table_.selected();
        if (index < 0)
        {
            set_status("Select an entry to delete.", true);
            return;
        }
        table_.remove(static_cast<std::size_t>(index));
        set_status("Entry deleted.", false);
    }

    void AddressListPanel::toggle_freeze_selected()
    {
        const int index = table_.selected();
        if (index < 0)
        {
            set_status("Select an entry to freeze.", true);
            return;
        }
        auto& entry  = table_.entries()[static_cast<std::size_t>(index)];
        entry.active = !entry.active;
        set_status(entry.active ? "Entry frozen." : "Entry unfrozen.", false);
    }

    void AddressListPanel::start_edit(std::size_t row, EditField field)
    {
        if (!table_.valid_index(row))
        {
            return;
        }
        editing_row_        = row;
        editing_field_      = field;
        edit_focus_pending_ = true;
        table_.set_selected(static_cast<int>(row));

        const auto&       entry = table_.entries()[row];
        const std::string text  = field == EditField::description ? entry.description : table_.display_value(row);
        std::snprintf(edit_buffer_.data(), edit_buffer_.size(), "%s", text.c_str());
    }

    void AddressListPanel::commit_edit()
    {
        if (editing_field_ == EditField::none)
        {
            return;
        }
        const std::size_t row   = editing_row_;
        const EditField   field = editing_field_;
        editing_field_          = EditField::none;
        edit_focus_pending_     = false;

        if (!table_.valid_index(row))
        {
            return;
        }
        if (field == EditField::description)
        {
            table_.entries()[row].description = edit_buffer_.data();
            set_status("Description updated.", false);
        }
        else
        {
            write_value_at(row, edit_buffer_.data());
        }
    }

    void AddressListPanel::write_value_at(std::size_t row, std::string_view text)
    {
        if (!table_.valid_index(row))
        {
            return;
        }
        if (!target_.valid())
        {
            set_status("Not attached; cannot write.", true);
            return;
        }
        if (write_pending_.has_value())
        {
            set_status("A write is already in progress.", true);
            return;
        }

        const auto& entry = table_.entries()[row];
        // Parse here as well so a malformed value keeps its detailed message.
        if (const auto parsed = scan::parse_value(entry.type, text, entry.hex); !parsed)
        {
            set_status("Value: " + parsed.error().message, true);
            return;
        }
        auto encoded = table_.encode_value(row, text);
        if (!encoded)
        {
            set_status("Value: invalid value.", true);
            return;
        }

        const std::uint64_t  entry_id = entry.id;
        const std::uint64_t  address  = entry.address;
        const process::JobId job_id   = worker_.next_job_id();
        write_pending_                = job_id;
        writing_entry_                = entry_id;

        const bool submitted = worker_.submit_write(
            job_id,
            entry_id,
            address,
            std::move(*encoded),
            [this, entry_id, job_id, cached = *encoded](process::JobResult&& result)
            {
                if (write_pending_ != job_id)
                {
                    return; // Superseded or shut down.
                }
                write_pending_.reset();
                writing_entry_.reset();

                const auto& write = std::get<process::WriteResult>(result);
                if (write.error)
                {
                    set_status(std::string("Write failed: ") + std::string(process::describe(*write.error)), true);
                    return;
                }
                table_.apply_write(entry_id, std::move(cached));
                set_status("Value written.", false);
            });
        if (!submitted)
        {
            write_pending_.reset();
            writing_entry_.reset();
            set_status("Write unavailable.", true);
        }
    }

    void AddressListPanel::draw()
    {
        draw_table();
        draw_footer();
        draw_file_popups();

        if (!status_.empty())
        {
            widgets::status_text(status_is_error_ ? widgets::StatusKind::error : widgets::StatusKind::info,
                                 status_.c_str());
        }
        else if (table_.empty())
        {
            widgets::status_text(widgets::StatusKind::info,
                                 "The address list is empty; double-click a Found row to add an entry.");
        }
    }

    void AddressListPanel::draw_table()
    {
        constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
                                        | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable
                                        | ImGuiTableFlags_SizingFixedFit;
        if (!ImGui::BeginTable("address_list", 5, flags))
        {
            return;
        }
        ImGui::TableSetupColumn("Active", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (std::size_t row = 0; row < table_.size(); ++row)
        {
            auto& entry = table_.entries()[row];
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(row));

            ImGui::TableSetColumnIndex(0);
            bool active = entry.active;
            if (ImGui::Checkbox("##active", &active))
            {
                entry.active = active;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Freeze this value");
            }

            ImGui::TableSetColumnIndex(1);
            if (editing_row_ == row && editing_field_ == EditField::description)
            {
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (edit_focus_pending_)
                {
                    ImGui::SetKeyboardFocusHere();
                }
                const bool finished = ImGui::InputText("##description_edit",
                                                       edit_buffer_.data(),
                                                       edit_buffer_.size(),
                                                       ImGuiInputTextFlags_EnterReturnsTrue);
                edit_focus_pending_ = false;
                if (finished || ImGui::IsItemDeactivated())
                {
                    commit_edit();
                }
            }
            else
            {
                const bool  selected = table_.selected() == static_cast<int>(row);
                const char* label    = entry.description.empty() ? " " : entry.description.c_str();
                if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(-FLT_MIN, 0.0f)))
                {
                    table_.set_selected(static_cast<int>(row));
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        start_edit(row, EditField::description);
                    }
                }
                if (ImGui::BeginPopupContextItem("##row_menu_description"))
                {
                    table_.set_selected(static_cast<int>(row));
                    draw_context_menu(row);
                    ImGui::EndPopup();
                }
            }

            ImGui::TableSetColumnIndex(2);
            {
                ui::ScopedMonoFont mono;
                ImGui::Text("0x%llX", static_cast<unsigned long long>(entry.address));
            }

            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(scan::describe(entry.type).data());

            ImGui::TableSetColumnIndex(4);
            if (editing_row_ == row && editing_field_ == EditField::value)
            {
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (edit_focus_pending_)
                {
                    ImGui::SetKeyboardFocusHere();
                }
                const bool finished = ImGui::InputText(
                    "##value_edit", edit_buffer_.data(), edit_buffer_.size(), ImGuiInputTextFlags_EnterReturnsTrue);
                edit_focus_pending_ = false;
                if (finished || ImGui::IsItemDeactivated())
                {
                    commit_edit();
                }
            }
            else if (writing_entry_.has_value() && *writing_entry_ == entry.id)
            {
                ImGui::TextDisabled("Writing...");
            }
            else
            {
                const std::string value    = table_.display_value(row);
                const bool        selected = table_.selected() == static_cast<int>(row);
                {
                    ui::ScopedMonoFont mono;
                    if (ImGui::Selectable(value.empty() ? " " : value.c_str(),
                                          selected,
                                          ImGuiSelectableFlags_AllowDoubleClick,
                                          ImVec2(-FLT_MIN, 0.0f)))
                    {
                        table_.set_selected(static_cast<int>(row));
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        {
                            start_edit(row, EditField::value);
                        }
                    }
                }
                if (ImGui::BeginPopupContextItem("##row_menu_value"))
                {
                    table_.set_selected(static_cast<int>(row));
                    draw_context_menu(row);
                    ImGui::EndPopup();
                }
            }

            ImGui::PopID();
        }

        ImGui::EndTable();

        if (pending_delete_ >= 0)
        {
            table_.remove(static_cast<std::size_t>(pending_delete_));
            pending_delete_ = -1;
            set_status("Entry deleted.", false);
        }
    }

    void AddressListPanel::draw_context_menu(std::size_t row)
    {
        if (!table_.valid_index(row))
        {
            return;
        }
        auto& entry = table_.entries()[row];

        if (ImGui::MenuItem("Change value"))
        {
            start_edit(row, EditField::value);
        }

        bool frozen = entry.active;
        if (ImGui::MenuItem("Freeze", nullptr, &frozen))
        {
            entry.active = frozen;
        }

        bool show_hex = entry.hex;
        if (ImGui::MenuItem("Show as hex", nullptr, &show_hex))
        {
            entry.hex = show_hex;
        }

        if (ImGui::MenuItem("Browse this memory region"))
        {
            browse_request_ = entry.address;
        }

        ImGui::Separator();

        ImGui::MenuItem("Find out what writes to this address", nullptr, false, false);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("Disabled: needs hardware watchpoints or ptrace");
        }

        ImGui::MenuItem("Group", nullptr, false, false);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("Disabled: address groups are not implemented");
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Delete"))
        {
            pending_delete_ = static_cast<int>(row);
        }
    }

    void AddressListPanel::draw_footer()
    {
        ImGui::Spacing();

        if (widgets::secondary_button("Advanced Options"))
        {
            ImGui::OpenPopup("##advanced_options");
        }
        const float extras_width = ImGui::CalcTextSize("Table Extras").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - extras_width));
        if (widgets::secondary_button("Table Extras"))
        {
            ImGui::OpenPopup("##table_extras");
        }

        if (ImGui::BeginPopup("##advanced_options"))
        {
            ImGui::TextDisabled("Advanced options are not implemented yet.");
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("##table_extras"))
        {
            ImGui::TextDisabled("Table extras are not implemented yet.");
            ImGui::EndPopup();
        }
    }

    void AddressListPanel::draw_file_popups()
    {
        if (open_requested_)
        {
            ImGui::OpenPopup("Open Table File");
            open_requested_ = false;
        }
        if (save_requested_)
        {
            ImGui::OpenPopup("Save Table File");
            save_requested_ = false;
        }

        if (ImGui::BeginPopupModal("Open Table File", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("Load an address table from a file.");
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24.0f);
            ImGui::InputTextWithHint("##open_path", "/path/to/table.txt", table_path_.data(), table_path_.size());
            if (widgets::primary_button("Open"))
            {
                if (const auto result = table::load(std::filesystem::path(table_path_.data()), table_); !result)
                {
                    set_status("Open failed: " + result.error(), true);
                }
                else
                {
                    set_status(std::format("Loaded {} entries.", table_.size()), false);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (widgets::secondary_button("Cancel"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("Save Table File", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("Save the address table to a file.");
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24.0f);
            ImGui::InputTextWithHint("##save_path", "/path/to/table.txt", table_path_.data(), table_path_.size());
            if (widgets::primary_button("Save"))
            {
                if (const auto result = table::save(std::filesystem::path(table_path_.data()), table_); !result)
                {
                    set_status("Save failed: " + result.error(), true);
                }
                else
                {
                    set_status(std::format("Saved {} entries.", table_.size()), false);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (widgets::secondary_button("Cancel"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

} // namespace slopkit::ui::panels
