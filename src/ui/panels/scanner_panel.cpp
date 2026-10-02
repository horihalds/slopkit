#include "ui/panels/scanner_panel.hpp"

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include <imgui.h>

#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"

namespace slopkit::ui::panels
{

    namespace
    {
        // A disabled control that explains why it is unavailable.
        void unavailable_checkbox(const char* label, bool* value, const char* reason)
        {
            ImGui::BeginDisabled();
            ImGui::Checkbox(label, value);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip("%s", reason);
            }
        }

        // The alignment the fast scan uses when the field is left blank.
        std::size_t default_alignment(scan::ValueType type) noexcept
        {
            const std::size_t size = scan::value_size(type);
            return size == 0 ? 4 : size;
        }
    } // namespace

    ScannerPanel::ScannerPanel(process::ProcessAccess& access, process::AttachedTarget& target)
        : access_(access), target_(target)
    {
    }

    scan::ScanType ScannerPanel::current_scan_type() const noexcept
    {
        return static_cast<scan::ScanType>(scan_type_);
    }

    scan::ValueType ScannerPanel::current_value_type() const noexcept
    {
        return static_cast<scan::ValueType>(value_type_);
    }

    void ScannerPanel::sync_worker_session()
    {
        const bool changed = target_.pid != worker_pid_ || target_.plugin_id != worker_plugin_;
        if (!changed)
        {
            return;
        }
        if (engine_.is_running())
        {
            engine_.cancel();
            return; // retry once the worker has stopped
        }

        worker_session_ = process::Session {};
        worker_pid_     = 0;
        worker_plugin_.clear();
        if (!target_.valid())
        {
            status_.clear();
            status_is_error_ = false;
            return;
        }

        worker_pid_    = target_.pid;
        worker_plugin_ = target_.plugin_id;

        auto attached = access_.attach(target_.pid, target_.plugin_id);
        if (!attached)
        {
            status_          = std::string("Scan session failed: ") + std::string(process::describe(attached.error()));
            status_is_error_ = true;
            return;
        }
        worker_session_  = std::move(*attached);
        status_          = std::string();
        status_is_error_ = false;
    }

    std::expected<scan::ScanConfig, std::string> ScannerPanel::build_config() const
    {
        scan::ScanConfig config;
        config.type       = current_scan_type();
        config.value_type = current_value_type();
        config.hex        = hex_;

        const bool wants_value = scan::needs_value(config.type) || config.type == scan::ScanType::value_between;
        if (wants_value)
        {
            auto value = scan::parse_value(config.value_type, value_buffer_.data(), hex_);
            if (!value)
            {
                return std::unexpected("Value: " + value.error().message);
            }
            config.value = std::move(*value);
        }
        if (config.type == scan::ScanType::value_between)
        {
            auto upper = scan::parse_value(config.value_type, value_upper_buffer_.data(), hex_);
            if (!upper)
            {
                return std::unexpected("Upper value: " + upper.error().message);
            }
            config.value_upper = std::move(*upper);
        }

        if (start_address_[0] != '\0')
        {
            auto start = scan::parse_address(start_address_.data());
            if (!start)
            {
                return std::unexpected("Start address: " + start.error().message);
            }
            config.filter.start = *start;
        }
        if (stop_address_[0] != '\0')
        {
            auto stop = scan::parse_address(stop_address_.data());
            if (!stop)
            {
                return std::unexpected("Stop address: " + stop.error().message);
            }
            config.filter.stop = *stop;
        }

        config.filter.writable      = writable_;
        config.filter.executable    = executable_;
        config.filter.copy_on_write = copy_on_write_;
        config.filter.alignment     = 1;
        if (fast_scan_)
        {
            const std::string_view text = alignment_.data();
            if (text.empty())
            {
                config.filter.alignment = default_alignment(config.value_type);
            }
            else
            {
                auto alignment = scan::parse_alignment(text);
                if (!alignment)
                {
                    return std::unexpected("Alignment: " + alignment.error().message);
                }
                config.filter.alignment = *alignment;
            }
        }
        return config;
    }

    void ScannerPanel::start_first_scan()
    {
        const auto config = build_config();
        if (!config)
        {
            status_          = config.error();
            status_is_error_ = true;
            return;
        }
        if (!worker_session_)
        {
            status_          = "No scan session; attach a process first.";
            status_is_error_ = true;
            return;
        }
        status_.clear();
        status_is_error_ = false;
        engine_.first_scan(*config, scan::make_session_source(worker_session_));
    }

    void ScannerPanel::start_next_scan()
    {
        const auto config = build_config();
        if (!config)
        {
            status_          = config.error();
            status_is_error_ = true;
            return;
        }
        if (!engine_.has_results())
        {
            status_          = "Run a first scan before refining.";
            status_is_error_ = true;
            return;
        }
        status_.clear();
        status_is_error_ = false;
        engine_.next_scan(*config);
    }

    void ScannerPanel::set_default_alignment(std::uint64_t alignment)
    {
        std::snprintf(alignment_.data(), alignment_.size(), "%llu", static_cast<unsigned long long>(alignment));
    }

    std::optional<std::uint64_t> ScannerPanel::take_memory_view_request()
    {
        auto request = memory_view_request_;
        memory_view_request_.reset();
        return request;
    }

    bool ScannerPanel::take_add_address_request()
    {
        const bool request   = add_address_request_;
        add_address_request_ = false;
        return request;
    }

    void ScannerPanel::draw()
    {
        sync_worker_session();

        const scan::ScanSnapshot snapshot    = engine_.snapshot();
        const bool               running     = snapshot.state == scan::ScanState::running;
        const bool               has_results = engine_.has_results();
        const bool               attached    = target_.valid() && static_cast<bool>(worker_session_);
        const scan::ScanType     scan_type   = current_scan_type();
        const bool wants_value = scan::needs_value(scan_type) || scan_type == scan::ScanType::value_between;

        widgets::section_header("Scan");

        const float hex_width =
            ImGui::CalcTextSize("Hex").x + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;

        ImGui::BeginDisabled(!wants_value);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - hex_width);
        {
            ui::ScopedMonoFont mono;
            ImGui::InputTextWithHint("##scan_value", "Value", value_buffer_.data(), value_buffer_.size());
        }
        if (scan_type == scan::ScanType::value_between)
        {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - hex_width);
            ui::ScopedMonoFont mono;
            ImGui::InputTextWithHint(
                "##scan_value_upper", "Upper value", value_upper_buffer_.data(), value_upper_buffer_.size());
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::Checkbox("Hex", &hex_);

        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::Combo(
            "##scan_type", &scan_type_, scan::kScanTypeNames, static_cast<int>(std::size(scan::kScanTypeNames)));
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::Combo(
            "##value_type", &value_type_, scan::kValueTypeNames, static_cast<int>(std::size(scan::kValueTypeNames)));

        ImGui::Spacing();

        // Scan actions.
        const float spacing     = ImGui::GetStyle().ItemSpacing.x;
        const float row_width   = ImGui::GetContentRegionAvail().x;
        const float first_width = row_width * 0.36f;
        const float next_width  = row_width * 0.32f;
        const float undo_width  = std::max(row_width - first_width - next_width - 2.0f * spacing, 1.0f);

        ImGui::BeginDisabled(!attached || running);
        if (widgets::primary_button(has_results ? "New Scan" : "First Scan", ImVec2(first_width, 0.0f)))
        {
            start_first_scan();
        }
        ImGui::SameLine();
        if (widgets::secondary_button("Next Scan", ImVec2(next_width, 0.0f)))
        {
            start_next_scan();
        }
        ImGui::SameLine();
        if (widgets::secondary_button("Undo Scan", ImVec2(undo_width, 0.0f)))
        {
            engine_.undo();
        }
        ImGui::EndDisabled();

        if (running)
        {
            ImGui::SameLine();
            if (widgets::secondary_button("Cancel"))
            {
                engine_.cancel();
            }
        }

        ImGui::Spacing();

        if (widgets::begin_group("memory_scan_options", "Memory Scan Options"))
        {
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
            ImGui::InputTextWithHint("##start", "Start address", start_address_.data(), start_address_.size());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##stop", "Stop address", stop_address_.data(), stop_address_.size());

            ImGui::Checkbox("Writable", &writable_);
            ImGui::SameLine();
            ImGui::Checkbox("Executable", &executable_);
            ImGui::SameLine();
            ImGui::Checkbox("CopyOnWrite", &copy_on_write_);

            ImGui::Checkbox("Fast Scan", &fast_scan_);
            ImGui::SameLine();
            ImGui::BeginDisabled(!fast_scan_);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
            ImGui::InputTextWithHint("##alignment", "Alignment", alignment_.data(), alignment_.size());
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip("Alignment step in bytes (decimal or 0x hex); blank uses the value size");
            }

            ImGui::Checkbox("Pause the game while scanning", &pause_while_scanning_);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Accepted as a setting; no effect until a plugin can suspend the target");
            }
        }
        widgets::end_group();

        ImGui::Spacing();

        ImGui::TextUnformatted("Extra options");
        unavailable_checkbox("Lua formula", &lua_formula_, "Disabled: a Lua interpreter is not a project dependency");
        ImGui::SameLine();
        unavailable_checkbox("Not", &not_operator_, "Disabled: the scan engine does not invert comparisons yet");
        ImGui::SameLine();
        unavailable_checkbox("Unrandomizer", &unrandomizer_, "Disabled: requires code injection");
        ImGui::SameLine();
        unavailable_checkbox("Enable Speedhack", &speedhack_, "Disabled: requires code injection");

        ImGui::Spacing();

        ImGui::BeginDisabled(!attached);
        if (widgets::secondary_button("Memory View"))
        {
            const auto address   = scan::parse_address(start_address_.data());
            memory_view_request_ = address.has_value() ? *address : std::uint64_t {0};
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("Open the Memory Viewer at the start address (or address 0)");
        }

        ImGui::SameLine();
        if (widgets::secondary_button("Add Address Manually"))
        {
            add_address_request_ = true;
        }

        ImGui::Spacing();

        if (!status_.empty())
        {
            widgets::status_text(status_is_error_ ? widgets::StatusKind::error : widgets::StatusKind::info,
                                 status_.c_str());
        }
        else if (!snapshot.message.empty())
        {
            widgets::status_text(snapshot.state == scan::ScanState::failed ? widgets::StatusKind::error
                                                                           : widgets::StatusKind::info,
                                 snapshot.message.c_str());
        }
        else if (!attached)
        {
            widgets::status_text(widgets::StatusKind::info, "Select a process to enable scanning.");
        }
    }

} // namespace slopkit::ui::panels
