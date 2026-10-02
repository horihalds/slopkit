#include "ui/panels/found_list_panel.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "scan/types.hpp"
#include "scan/value.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"

namespace slopkit::ui::panels
{

    FoundListPanel::FoundListPanel(scan::ScanEngine& engine, table::AddressTable& table)
        : engine_(engine), table_(table)
    {
    }

    void FoundListPanel::add_to_table(const scan::ScanHit& hit, const scan::ScanConfig& config)
    {
        table::AddressEntry entry;
        entry.address = hit.address;
        entry.type    = config.value_type;
        entry.bytes   = hit.value;
        entry.hex     = config.hex;
        table_.add(std::move(entry));
    }

    void FoundListPanel::draw()
    {
        const scan::ScanSnapshot snapshot = engine_.snapshot();
        const scan::ScanConfig   config   = engine_.config();

        const std::string header = std::format("Found: {}", snapshot.hit_count);
        widgets::section_header(header.c_str());

        if (snapshot.hits.size() < snapshot.hit_count)
        {
            std::string note =
                std::format("Showing the first {} of {} matches", snapshot.hits.size(), snapshot.hit_count);
            if (snapshot.truncated)
            {
                note += " (result cap reached)";
            }
            widgets::status_text(widgets::StatusKind::info, note.c_str());
        }
        else if (snapshot.truncated)
        {
            widgets::status_text(widgets::StatusKind::warning,
                                 "The result cap was reached; only the first matches are stored.");
        }

        constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
                                        | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable
                                        | ImGuiTableFlags_Sortable;
        if (!ImGui::BeginTable("found_list", 3, flags))
        {
            return;
        }
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Previous", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        const auto&      hits = snapshot.hits;
        std::vector<int> order(hits.size());
        for (int i = 0; i < static_cast<int>(order.size()); ++i)
        {
            order[static_cast<std::size_t>(i)] = i;
        }

        int  sort_column = 0;
        bool ascending   = true;
        if (const ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs != nullptr && specs->SpecsCount > 0)
        {
            sort_column = specs->Specs[0].ColumnIndex;
            ascending   = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
        }

        std::ranges::sort(order,
                          [&](int lhs, int rhs)
                          {
                              const auto& left     = hits[static_cast<std::size_t>(lhs)];
                              const auto& right    = hits[static_cast<std::size_t>(rhs)];
                              int         order_by = 0;
                              switch (sort_column)
                              {
                              case 1:
                                  order_by = left.value < right.value ? -1 : (right.value < left.value ? 1 : 0);
                                  break;
                              case 2:
                                  order_by =
                                      left.previous < right.previous ? -1 : (right.previous < left.previous ? 1 : 0);
                                  break;
                              default:
                                  order_by = left.address < right.address ? -1 : (left.address > right.address ? 1 : 0);
                                  break;
                              }
                              if (order_by == 0)
                              {
                                  order_by = left.address < right.address ? -1 : (left.address > right.address ? 1 : 0);
                              }
                              return ascending ? order_by < 0 : order_by > 0;
                          });

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(order.size()));
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                const int   index = order[static_cast<std::size_t>(row)];
                const auto& hit   = hits[static_cast<std::size_t>(index)];

                ImGui::TableNextRow();
                ImGui::PushID(index);

                ImGui::TableSetColumnIndex(0);
                const std::string address = std::format("0x{:X}", hit.address);
                {
                    ui::ScopedMonoFont mono;
                    if (ImGui::Selectable(address.c_str(),
                                          selected_ == index,
                                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                    {
                        selected_ = index;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        {
                            add_to_table(hit, config);
                        }
                    }
                }

                ImGui::TableSetColumnIndex(1);
                {
                    ui::ScopedMonoFont mono;
                    ImGui::TextUnformatted(scan::format_value(config.value_type, hit.value, config.hex).c_str());
                }

                ImGui::TableSetColumnIndex(2);
                if (!hit.previous.empty())
                {
                    ui::ScopedMonoFont mono;
                    ImGui::TextUnformatted(scan::format_value(config.value_type, hit.previous, config.hex).c_str());
                }

                ImGui::PopID();
            }
        }

        ImGui::EndTable();
    }

} // namespace slopkit::ui::panels
