#include "table/address_table.hpp"

#include <algorithm>
#include <format>
#include <iterator>
#include <optional>
#include <unordered_map>
#include <utility>

#include "core/log.hpp"
#include "core/log_categories.hpp"

namespace slopkit::table
{

    void AddressTable::add(AddressEntry entry)
    {
        entry.id = next_id_++;
        if (entry.kind == EntryKind::script)
        {
            log::info(
                log::category::table,
                std::format(
                    "script entry added: id {} '{}' ({} byte(s))", entry.id, entry.description, entry.script.size()));
        }
        else
        {
            log::info(
                log::category::table,
                std::format("entry added: id {} at {:X} ({} byte(s))", entry.id, entry.address, entry.bytes.size()));
        }
        entries_.push_back(std::move(entry));
        selected_ = static_cast<int>(entries_.size()) - 1;
    }

    std::size_t AddressTable::add_script(std::string description, std::string script)
    {
        AddressEntry entry;
        entry.kind        = EntryKind::script;
        entry.description = std::move(description);
        entry.script      = std::move(script);
        add(std::move(entry));
        return entries_.size() - 1;
    }

    bool AddressTable::set_script(std::size_t index, std::string script)
    {
        if (!valid_index(index) || entries_[index].kind != EntryKind::script)
        {
            return false;
        }
        log::info(log::category::table, std::format("script edited at index {} ({} byte(s))", index, script.size()));
        entries_[index].script = std::move(script);
        return true;
    }

    bool AddressTable::set_description(std::size_t index, std::string description)
    {
        if (!valid_index(index))
        {
            return false;
        }
        log::info(log::category::table, std::format("entry description changed at index {}", index));
        entries_[index].description = std::move(description);
        return true;
    }

    void AddressTable::remove(std::size_t index)
    {
        if (index >= entries_.size())
        {
            return;
        }
        const std::size_t count = subtree_size(index);
        log::info(log::category::table, std::format("entry removed at index {} ({} row(s))", index, count));
        const std::pair<std::size_t, std::size_t> block {index, count};
        erase_blocks(std::span<const std::pair<std::size_t, std::size_t>>(&block, 1));
    }

    void AddressTable::remove_rows(std::span<const std::size_t> rows)
    {
        std::vector<std::size_t> sorted(rows.begin(), rows.end());
        std::ranges::sort(sorted);
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

        // Resolve the listed rows to their top-level blocks: a row inside an
        // earlier block is already covered by it and is skipped, so a child is
        // never removed twice.
        std::vector<std::pair<std::size_t, std::size_t>> blocks;
        std::size_t                                      row_count = 0;
        for (const std::size_t row : sorted)
        {
            if (row >= entries_.size())
            {
                continue;
            }
            if (std::ranges::any_of(blocks,
                                    [row](const std::pair<std::size_t, std::size_t>& block)
                                    {
                                        return row >= block.first && row < block.first + block.second;
                                    }))
            {
                continue;
            }
            const std::size_t count = subtree_size(row);
            blocks.emplace_back(row, count);
            row_count += count;
        }
        if (blocks.empty())
        {
            return;
        }

        log::info(log::category::table,
                  std::format("entries removed ({} row(s) in {} block(s))", row_count, blocks.size()));
        erase_blocks(blocks);
    }

    void AddressTable::erase_blocks(std::span<const std::pair<std::size_t, std::size_t>> blocks)
    {
        if (blocks.empty())
        {
            return;
        }

        // Erase from the back so the earlier positions stay valid.
        for (auto block = blocks.rbegin(); block != blocks.rend(); ++block)
        {
            entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(block->first),
                           entries_.begin() + static_cast<std::ptrdiff_t>(block->first + block->second));
        }

        const auto new_size = static_cast<int>(entries_.size());

        // The row `index` in the new vector: nullopt when it lay inside a block,
        // otherwise its position.
        const auto map_row = [blocks](std::size_t index) -> std::optional<std::size_t>
        {
            std::size_t erased = 0;
            for (const auto& [start, count] : blocks)
            {
                if (index < start)
                {
                    return index - erased;
                }
                if (index < start + count)
                {
                    return std::nullopt;
                }
                erased += count;
            }
            return index - erased;
        };

        if (selected_ >= 0)
        {
            if (const auto mapped = map_row(static_cast<std::size_t>(selected_)))
            {
                selected_ = static_cast<int>(*mapped);
            }
            else
            {
                // The cursor was inside a block: it lands on that block's freed
                // slot, clamped to the last row.
                const std::size_t cursor        = static_cast<std::size_t>(selected_);
                std::size_t       erased_before = 0;
                std::size_t       slot          = 0;
                for (const auto& [start, count] : blocks)
                {
                    if (cursor >= start && cursor < start + count)
                    {
                        slot = start - erased_before;
                        break;
                    }
                    erased_before += count;
                }
                selected_ = new_size == 0 ? -1 : std::min(static_cast<int>(slot), new_size - 1);
            }
        }

        std::vector<std::size_t> kept;
        kept.reserve(selected_rows_.size());
        for (const std::size_t row : selected_rows_)
        {
            if (const auto mapped = map_row(row))
            {
                kept.push_back(*mapped);
            }
        }
        selected_rows_ = std::move(kept);
    }

    void AddressTable::clear()
    {
        if (!entries_.empty())
        {
            log::info(log::category::table, std::format("table cleared ({} entry/entries)", entries_.size()));
        }
        entries_.clear();
        selected_ = -1;
        selected_rows_.clear();
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
        selected_rows_.clear();
    }

    void AddressTable::move(std::size_t from, std::size_t to)
    {
        if (from >= entries_.size() || to >= entries_.size())
        {
            return;
        }
        // `to` is the moved row's final index, so a downward move inserts in
        // front of the row that follows it; an upward move in front of `to`
        // itself. Both adopt the landing row's level, so a subtree moves whole.
        move_row(from, from < to ? to + 1 : to, false);
    }

    std::size_t AddressTable::depth_of(std::size_t index) const
    {
        if (index >= entries_.size())
        {
            return 0;
        }

        std::size_t   depth   = 0;
        std::size_t   current = index;
        std::uint64_t parent  = entries_[current].parent;
        while (parent != 0)
        {
            // A parent always precedes its child, so only look before `current`;
            // that also makes a corrupt cycle terminate.
            std::size_t found = entries_.size();
            for (std::size_t i = 0; i < current; ++i)
            {
                if (entries_[i].id == parent)
                {
                    found = i;
                    break;
                }
            }
            if (found == entries_.size())
            {
                break;
            }
            ++depth;
            current = found;
            parent  = entries_[current].parent;
        }
        return depth;
    }

    std::size_t AddressTable::subtree_size(std::size_t index) const
    {
        if (index >= entries_.size())
        {
            return 0;
        }

        // Depth-first order keeps every descendant directly after the root, so
        // the subtree lasts until the next row at the root's depth or above.
        const std::size_t root_depth = depth_of(index);
        std::size_t       count      = 1;
        for (std::size_t i = index + 1; i < entries_.size(); ++i)
        {
            if (depth_of(i) <= root_depth)
            {
                break;
            }
            ++count;
        }
        return count;
    }

    bool AddressTable::is_within_subtree(std::size_t index, std::size_t ancestor) const
    {
        if (index >= entries_.size() || ancestor >= entries_.size())
        {
            return false;
        }
        return index >= ancestor && index < ancestor + subtree_size(ancestor);
    }

    std::size_t
    AddressTable::moved_row(std::size_t index, std::size_t from, std::size_t count, std::size_t insertion) noexcept
    {
        if (index >= from && index < from + count)
        {
            return insertion + (index - from);
        }
        const std::size_t rest = index < from ? index : index - count;
        return rest < insertion ? rest : rest + count;
    }

    bool AddressTable::move_row(std::size_t from, std::size_t target, bool nest)
    {
        if (from >= entries_.size())
        {
            return false;
        }
        if (nest ? target >= entries_.size() : target > entries_.size())
        {
            return false;
        }
        // A drop onto the moved row, one of its descendants or an edge inside
        // its own subtree would break the tree; refuse it.
        if (target < entries_.size() && is_within_subtree(target, from))
        {
            return false;
        }

        const std::size_t   count      = subtree_size(from);
        const std::uint64_t old_parent = entries_[from].parent;

        // Where the block lands: an element position in the original vector and
        // the parent id the moved root adopts there.
        std::size_t   before     = 0;
        std::uint64_t new_parent = 0;
        if (nest)
        {
            before     = target + subtree_size(target);
            new_parent = entries_[target].id;
        }
        else if (target == entries_.size())
        {
            // Past the last row: the new row joins the last row's level.
            before     = entries_.size();
            new_parent = entries_.back().parent;
        }
        else
        {
            before     = target;
            new_parent = entries_[target].parent;
        }

        // The extraction shifts every position after `from` down by `count`.
        const std::size_t insertion = from < before ? before - count : before;
        if (insertion == from && new_parent == old_parent)
        {
            return false;
        }

        std::vector<AddressEntry> block(
            std::make_move_iterator(entries_.begin() + static_cast<std::ptrdiff_t>(from)),
            std::make_move_iterator(entries_.begin() + static_cast<std::ptrdiff_t>(from + count)));
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(from),
                       entries_.begin() + static_cast<std::ptrdiff_t>(from + count));
        entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(insertion),
                        std::make_move_iterator(block.begin()),
                        std::make_move_iterator(block.end()));
        entries_[insertion].parent = new_parent;

        // The selection rides on the moved block; every row the block jumped
        // over shifts the other way. The mapping is the same one the cursor
        // always got, applied to the cursor and to every selected row.
        if (selected_ >= 0)
        {
            selected_ = static_cast<int>(moved_row(static_cast<std::size_t>(selected_), from, count, insertion));
        }
        for (std::size_t& row : selected_rows_)
        {
            row = moved_row(row, from, count, insertion);
        }
        // A move is a bijection, so the rows stay unique, but their order can
        // break (a row ahead of the block may land behind it).
        std::ranges::sort(selected_rows_);

        log::info(log::category::table,
                  std::format("entry moved from {} to {}{}", from, insertion, nest ? " (nested)" : ""));
        return true;
    }

    MergeSummary AddressTable::merge(std::span<const AddressEntry> incoming)
    {
        MergeSummary summary;

        const auto matches = [](const AddressEntry& candidate, const AddressEntry& entry)
        {
            if (candidate.kind != entry.kind)
            {
                return false;
            }
            if (entry.kind == EntryKind::script)
            {
                return candidate.description == entry.description && candidate.script == entry.script;
            }
            return candidate.address == entry.address && candidate.type == entry.type
                && candidate.description == entry.description;
        };

        // incoming id -> the id it became here, so an incoming child is
        // re-pointed at its merged copy, or at the existing entry a skipped
        // duplicate resolved to. An unknown (or unset) parent becomes top level.
        std::unordered_map<std::uint64_t, std::uint64_t>   id_map;
        std::vector<std::pair<std::size_t, std::uint64_t>> pending; // (row, incoming parent id)

        for (const auto& entry : incoming)
        {
            if (const auto found = std::ranges::find_if(entries_,
                                                        [&](const AddressEntry& candidate)
                                                        {
                                                            return matches(candidate, entry);
                                                        });
                found != entries_.end())
            {
                ++summary.skipped;
                if (entry.id != 0)
                {
                    id_map[entry.id] = found->id;
                }
                continue;
            }

            AddressEntry copy = entry;
            copy.id           = next_id_++;
            copy.parent       = 0;
            if (entry.id != 0)
            {
                id_map[entry.id] = copy.id;
            }
            const std::size_t row = entries_.size();
            entries_.push_back(std::move(copy));
            pending.emplace_back(row, entry.parent);
            ++summary.added;
        }

        for (const auto& [row, incoming_parent] : pending)
        {
            if (incoming_parent == 0)
            {
                continue;
            }
            if (const auto found = id_map.find(incoming_parent); found != id_map.end())
            {
                entries_[row].parent = found->second;
            }
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

    std::span<const std::size_t> AddressTable::selected_rows() const noexcept
    {
        return selected_rows_;
    }

    void AddressTable::set_selected_rows(std::span<const std::size_t> rows) noexcept
    {
        selected_rows_.assign(rows.begin(), rows.end());
        std::ranges::sort(selected_rows_);
        selected_rows_.erase(std::unique(selected_rows_.begin(), selected_rows_.end()), selected_rows_.end());
        selected_rows_.erase(std::remove_if(selected_rows_.begin(),
                                            selected_rows_.end(),
                                            [this](std::size_t row)
                                            {
                                                return row >= entries_.size();
                                            }),
                             selected_rows_.end());
    }

    bool AddressTable::is_row_selected(std::size_t index) const noexcept
    {
        return std::ranges::binary_search(selected_rows_, index);
    }

    std::string AddressTable::display_value(std::size_t index) const
    {
        if (!valid_index(index) || entries_[index].kind != EntryKind::value)
        {
            return {};
        }
        const auto& entry = entries_[index];
        return scan::format_value(entry.type, entry.bytes, entry.hex);
    }

    std::expected<std::vector<std::byte>, process::AccessError> AddressTable::encode_value(std::size_t      index,
                                                                                           std::string_view text) const
    {
        if (!valid_index(index) || entries_[index].kind != EntryKind::value)
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
            if (entry.kind != EntryKind::value || !entry.active || entry.bytes.empty())
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
