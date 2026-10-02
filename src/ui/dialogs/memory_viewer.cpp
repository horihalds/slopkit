#include "ui/dialogs/memory_viewer.hpp"

#include <cstddef>
#include <cstdio>
#include <format>
#include <string>

#include <imgui.h>

#include "scan/value.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"

namespace slopkit::ui::dialogs
{

    namespace
    {
        // Rows of 16 bytes shown per page.
        constexpr std::uint64_t kRowBytes = 16;
        constexpr std::size_t   kRows     = 24;

        bool is_printable(std::byte value)
        {
            const auto character = static_cast<unsigned char>(value);
            return character >= 0x20 && character < 0x7F;
        }
    } // namespace

    MemoryViewer::MemoryViewer(process::AttachedTarget& target) : target_(target)
    {
        set_address(0);
    }

    void MemoryViewer::set_address(std::uint64_t address)
    {
        base_ = address & ~static_cast<std::uint64_t>(kRowBytes - 1);
        std::snprintf(goto_buffer_.data(), goto_buffer_.size(), "0x%llX", static_cast<unsigned long long>(base_));
    }

    void MemoryViewer::draw(bool& open)
    {
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 52.0f, ImGui::GetFontSize() * 26.0f),
                                 ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Memory Viewer", &open))
        {
            ImGui::End();
            return;
        }

        if (!target_.valid())
        {
            widgets::status_text(widgets::StatusKind::info, "No process attached.");
            ImGui::End();
            return;
        }

        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        const bool submitted = ImGui::InputTextWithHint(
            "##goto", "address", goto_buffer_.data(), goto_buffer_.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        const bool go = widgets::secondary_button("Go");
        ImGui::SameLine();
        const bool previous = widgets::secondary_button("<");
        ImGui::SameLine();
        const bool next = widgets::secondary_button(">");

        if (submitted || go)
        {
            if (const auto parsed = scan::parse_address(goto_buffer_.data()); parsed.has_value())
            {
                set_address(*parsed);
            }
        }
        if (previous)
        {
            const std::uint64_t page = kRowBytes * kRows;
            set_address(base_ >= page ? base_ - page : 0);
        }
        if (next)
        {
            set_address(base_ + kRowBytes * kRows);
        }

        widgets::section_header("Hex dump");

        bool any_unreadable = false;
        {
            ui::ScopedMonoFont mono;
            for (std::size_t row = 0; row < kRows; ++row)
            {
                const std::uint64_t address = base_ + row * kRowBytes;
                const auto          data    = target_.session.read(address, kRowBytes);

                std::string line = std::format("0x{:016X}  ", address);
                std::string ascii;
                if (data && !data->empty())
                {
                    for (std::size_t i = 0; i < kRowBytes; ++i)
                    {
                        if (i < data->size())
                        {
                            line += std::format("{:02X} ", static_cast<unsigned>((*data)[i]));
                            ascii += is_printable((*data)[i]) ? static_cast<char>((*data)[i]) : '.';
                        }
                        else
                        {
                            line += "   ";
                            ascii += '.';
                        }
                    }
                }
                else
                {
                    any_unreadable = true;
                    line += std::string(kRowBytes * 3, ' ');
                    ascii = std::string(kRowBytes, '?');
                }
                line += ' ';
                line += ascii;
                ImGui::TextUnformatted(line.c_str());
            }
        }

        if (any_unreadable)
        {
            widgets::status_text(widgets::StatusKind::warning,
                                 "Some rows could not be read; their bytes are marked with '?'.");
        }

        ImGui::End();
    }

} // namespace slopkit::ui::dialogs
