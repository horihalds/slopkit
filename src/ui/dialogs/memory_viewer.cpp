#include "ui/dialogs/memory_viewer.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <string>
#include <utility>

#include <imgui.h>

#include "scan/value.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"

namespace slopkit::ui::dialogs
{

    namespace
    {
        bool is_printable(std::byte value)
        {
            const auto character = static_cast<unsigned char>(value);
            return character >= 0x20 && character < 0x7F;
        }
    } // namespace

    MemoryViewer::MemoryViewer(process::AccessWorker& worker, process::AttachedTarget& target)
        : worker_(worker), target_(target)
    {
        set_address(0);
    }

    void MemoryViewer::set_address(std::uint64_t address)
    {
        base_ = address & ~static_cast<std::uint64_t>(kRowBytes - 1);
        std::snprintf(goto_buffer_.data(), goto_buffer_.size(), "0x%llX", static_cast<unsigned long long>(base_));
    }

    void MemoryViewer::request_page(double now)
    {
        if (pending_.has_value() || !target_.valid())
        {
            return;
        }

        const std::uint64_t      base = base_;
        const process::ProcessId pid  = target_.pid;

        const process::JobId job_id = worker_.next_job_id();
        pending_                    = job_id;
        requested_base_             = base;
        requested_pid_              = pid;
        refresh_requested_          = false;
        next_refresh_time_          = now + 0.5;

        const bool submitted = worker_.submit_read(job_id,
                                                   base,
                                                   kRowBytes * kRows,
                                                   [this, base, pid, job_id](process::JobResult&& result)
                                                   {
                                                       if (pending_ != job_id)
                                                       {
                                                           return; // Superseded or shut down.
                                                       }
                                                       pending_.reset();

                                                       // Drop a page whose target or base
                                                       // changed while it was in flight.
                                                       if (!target_.valid() || target_.pid != pid || base_ != base)
                                                       {
                                                           return;
                                                       }

                                                       auto& read = std::get<process::ReadResult>(result);
                                                       if (read.error || read.bytes.empty())
                                                       {
                                                           page_bytes_.clear();
                                                           unreadable_.fill(true);
                                                           return;
                                                       }

                                                       page_bytes_ = std::move(read.bytes);
                                                       for (std::size_t row = 0; row < kRows; ++row)
                                                       {
                                                           unreadable_[row] = row * kRowBytes >= page_bytes_.size();
                                                       }
                                                   });
        if (!submitted)
        {
            pending_.reset();
        }
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
        ImGui::SameLine();
        if (widgets::secondary_button("Refresh"))
        {
            refresh_requested_ = true;
        }

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

        // Request a page when the base or the attached target changed, on the
        // Refresh button and on a 0.5 s interval, but never while one is in
        // flight.
        const double now = ImGui::GetTime();
        if (!pending_.has_value()
            && (refresh_requested_ || target_.pid != requested_pid_ || base_ != requested_base_
                || now >= next_refresh_time_))
        {
            request_page(now);
        }

        widgets::section_header("Hex dump");
        if (pending_.has_value())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Loading...");
        }

        bool any_unreadable = false;
        {
            ui::ScopedMonoFont mono;
            for (std::size_t row = 0; row < kRows; ++row)
            {
                const std::uint64_t address = base_ + row * kRowBytes;
                const std::size_t   offset  = row * kRowBytes;

                std::string line = std::format("0x{:016X}  ", address);
                std::string ascii;
                if (unreadable_[row] || offset >= page_bytes_.size())
                {
                    any_unreadable = true;
                    line += std::string(kRowBytes * 3, ' ');
                    ascii = std::string(kRowBytes, '?');
                }
                else
                {
                    for (std::size_t i = 0; i < kRowBytes; ++i)
                    {
                        if (offset + i < page_bytes_.size())
                        {
                            line += std::format("{:02X} ", static_cast<unsigned>(page_bytes_[offset + i]));
                            ascii += is_printable(page_bytes_[offset + i]) ? static_cast<char>(page_bytes_[offset + i])
                                                                           : '.';
                        }
                        else
                        {
                            line += "   ";
                            ascii += '.';
                        }
                    }
                }
                line += ' ';
                line += ascii;
                ImGui::TextUnformatted(line.c_str());
            }
        }

        if (any_unreadable && !pending_.has_value())
        {
            widgets::status_text(widgets::StatusKind::warning,
                                 "Some rows could not be read; their bytes are marked with '?'.");
        }

        ImGui::End();
    }

} // namespace slopkit::ui::dialogs
