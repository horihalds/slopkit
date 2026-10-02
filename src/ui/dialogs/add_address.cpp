#include "ui/dialogs/add_address.hpp"

#include <cfloat>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <imgui.h>

#include "scan/types.hpp"
#include "scan/value.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"

namespace slopkit::ui::dialogs
{

    namespace
    {
        constexpr std::size_t kMaxSize = 4096;

        bool parse_size(const char* text, std::size_t& out)
        {
            const std::string_view view(text);
            if (view.empty())
            {
                return false;
            }
            std::uint64_t value     = 0;
            const auto [end, error] = std::from_chars(view.data(), view.data() + view.size(), value);
            if (error != std::errc {} || end != view.data() + view.size() || value == 0 || value > kMaxSize)
            {
                return false;
            }
            out = static_cast<std::size_t>(value);
            return true;
        }
    } // namespace

    AddAddress::AddAddress(table::AddressTable& table) : table_(table) {}

    void AddAddress::commit()
    {
        const auto address = scan::parse_address(address_.data());
        if (!address)
        {
            status_ = "Address: " + address.error().message;
            return;
        }

        const auto  type    = static_cast<scan::ValueType>(type_index_);
        const bool  dynamic = type == scan::ValueType::string || type == scan::ValueType::byte_array;
        std::size_t size    = scan::value_size(type);
        if (type == scan::ValueType::all)
        {
            size = 4;
        }
        if (dynamic && !parse_size(size_.data(), size))
        {
            status_ = "Size must be a number between 1 and 4096.";
            return;
        }

        table::AddressEntry entry;
        entry.description = description_.data();
        entry.address     = *address;
        entry.type        = type;
        entry.hex         = hex_;
        entry.bytes.assign(size, std::byte {0});
        table_.add(std::move(entry));
        status_ = "Address added.";
    }

    void AddAddress::draw(bool& open)
    {
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 28.0f, ImGui::GetFontSize() * 16.0f),
                                 ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Add Address", &open))
        {
            ImGui::End();
            return;
        }

        ImGui::TextUnformatted("Description");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##description", description_.data(), description_.size());

        ImGui::TextUnformatted("Address");
        ImGui::SetNextItemWidth(-FLT_MIN);
        {
            ui::ScopedMonoFont mono;
            ImGui::InputTextWithHint("##address", "0x1234", address_.data(), address_.size());
        }

        ImGui::TextUnformatted("Type");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::Combo("##type", &type_index_, scan::kValueTypeNames, static_cast<int>(std::size(scan::kValueTypeNames)));

        const auto type = static_cast<scan::ValueType>(type_index_);
        if (type == scan::ValueType::string || type == scan::ValueType::byte_array)
        {
            ImGui::TextUnformatted("Size (bytes)");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##size", "32", size_.data(), size_.size());
        }

        ImGui::Checkbox("Show as hex", &hex_);

        ImGui::Spacing();
        if (widgets::primary_button("Add"))
        {
            commit();
        }
        ImGui::SameLine();
        if (widgets::secondary_button("Close"))
        {
            open = false;
        }

        if (!status_.empty())
        {
            widgets::status_text(widgets::StatusKind::info, status_.c_str());
        }

        ImGui::End();
    }

} // namespace slopkit::ui::dialogs
