#include "table/address_table.hpp"

#include <algorithm>
#include <utility>

namespace slopkit::table
{

    void AddressTable::add(AddressEntry entry)
    {
        entry.id = next_id_++;
        entries_.push_back(std::move(entry));
        selected_ = static_cast<int>(entries_.size()) - 1;
    }

    void AddressTable::remove(std::size_t index)
    {
        if (index >= entries_.size())
        {
            return;
        }
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
        if (selected_ == static_cast<int>(index))
        {
            selected_ =
                entries_.empty() ? -1 : std::min(static_cast<int>(index), static_cast<int>(entries_.size()) - 1);
        }
        else if (selected_ > static_cast<int>(index))
        {
            --selected_;
        }
    }

    void AddressTable::clear()
    {
        entries_.clear();
        selected_ = -1;
    }

    std::span<AddressEntry> AddressTable::entries() noexcept
    {
        return entries_;
    }

    std::span<const AddressEntry> AddressTable::entries() const noexcept
    {
        return entries_;
    }

    std::size_t AddressTable::size() const noexcept
    {
        return entries_.size();
    }

    bool AddressTable::empty() const noexcept
    {
        return entries_.empty();
    }

    bool AddressTable::valid_index(std::size_t index) const noexcept
    {
        return index < entries_.size();
    }

    int AddressTable::selected() const noexcept
    {
        return selected_;
    }

    void AddressTable::set_selected(int index) noexcept
    {
        selected_ = index >= 0 && index < static_cast<int>(entries_.size()) ? index : -1;
    }

    std::string AddressTable::display_value(std::size_t index) const
    {
        if (!valid_index(index))
        {
            return {};
        }
        const auto& entry = entries_[index];
        return scan::format_value(entry.type, entry.bytes, entry.hex);
    }

    std::expected<std::vector<std::byte>, process::AccessError> AddressTable::encode_value(std::size_t      index,
                                                                                           std::string_view text) const
    {
        if (!valid_index(index))
        {
            return std::unexpected(process::AccessError::invalid_argument);
        }
        const auto& entry = entries_[index];

        auto parsed = scan::parse_value(entry.type, text, entry.hex);
        if (!parsed)
        {
            return std::unexpected(process::AccessError::invalid_argument);
        }
        auto bytes = scan::encode_value(entry.type, *parsed);
        if (bytes.empty())
        {
            return std::unexpected(process::AccessError::invalid_argument);
        }
        return bytes;
    }

    std::vector<process::WriteItem> AddressTable::freeze_items(double now, double interval)
    {
        std::vector<process::WriteItem> items;
        if (now < next_freeze_time_)
        {
            return items;
        }
        next_freeze_time_ = now + (interval > 0.0 ? interval : 0.1);

        for (const auto& entry : entries_)
        {
            if (!entry.active || entry.bytes.empty())
            {
                continue;
            }
            items.push_back(process::WriteItem {.id = entry.id, .address = entry.address, .bytes = entry.bytes});
        }
        return items;
    }

    void AddressTable::apply_write(std::uint64_t entry_id, std::vector<std::byte> bytes)
    {
        for (auto& entry : entries_)
        {
            if (entry.id == entry_id)
            {
                entry.bytes = std::move(bytes);
                return;
            }
        }
    }

} // namespace slopkit::table
