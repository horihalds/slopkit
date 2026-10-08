#include "table/address_table.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "core/log.hpp"
#include "core/log_categories.hpp"

namespace slopkit::table
{

    void AddressTable::add(AddressEntry entry)
    {
        entry.id = next_id_++;
        log::info(log::category::table,
                  std::format("entry added: id {} at {:X} ({} byte(s))", entry.id, entry.address, entry.bytes.size()));
        entries_.push_back(std::move(entry));
        selected_ = static_cast<int>(entries_.size()) - 1;
    }

    void AddressTable::remove(std::size_t index)
    {
        if (index >= entries_.size())
        {
            return;
        }
        log::info(log::category::table, std::format("entry removed at index {}", index));
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
        if (!entries_.empty())
        {
            log::info(log::category::table, std::format("table cleared ({} entry/entries)", entries_.size()));
        }
        entries_.clear();
        selected_ = -1;
    }

    void AddressTable::replace(std::vector<AddressEntry> entries)
    {
        entries_ = std::move(entries);
        next_id_ = 1;
        for (auto& entry : entries_)
        {
            entry.id = next_id_++;
        }
        selected_ = entries_.empty() ? -1 : static_cast<int>(entries_.size()) - 1;
    }

    void AddressTable::move(std::size_t from, std::size_t to)
    {
        if (from == to || from >= entries_.size() || to >= entries_.size())
        {
            return;
        }

        AddressEntry moving = std::move(entries_[from]);
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(from));
        entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(to), std::move(moving));

        // The selection follows the moved row; the rows it jumped over shift by
        // one the other way.
        if (selected_ == static_cast<int>(from))
        {
            selected_ = static_cast<int>(to);
        }
        else if (from < to && selected_ > static_cast<int>(from) && selected_ <= static_cast<int>(to))
        {
            --selected_;
        }
        else if (from > to && selected_ >= static_cast<int>(to) && selected_ < static_cast<int>(from))
        {
            ++selected_;
        }

        log::info(log::category::table, std::format("entry moved from {} to {}", from, to));
    }

    MergeSummary AddressTable::merge(std::span<const AddressEntry> incoming)
    {
        MergeSummary summary;
        for (const auto& entry : incoming)
        {
            const bool exists = std::ranges::any_of(entries_,
                                                    [&](const AddressEntry& candidate)
                                                    {
                                                        return candidate.address == entry.address
                                                            && candidate.type == entry.type
                                                            && candidate.description == entry.description;
                                                    });
            if (exists)
            {
                ++summary.skipped;
                continue;
            }

            AddressEntry copy = entry;
            copy.id           = next_id_++;
            entries_.push_back(std::move(copy));
            ++summary.added;
        }
        if (summary.added > 0)
        {
            log::info(log::category::table,
                      std::format("merged {} entry/entries ({} skipped)", summary.added, summary.skipped));
        }
        return summary;
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

    TableSettings& AddressTable::settings() noexcept
    {
        return settings_;
    }

    const TableSettings& AddressTable::settings() const noexcept
    {
        return settings_;
    }

} // namespace slopkit::table
