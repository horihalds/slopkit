#include "debug/breakpoints.hpp"

#include <algorithm>
#include <format>

namespace slopkit::debug
{
    namespace
    {
        template<typename Predicate>
        std::optional<std::uint32_t>
        first_free_slot(std::span<const Breakpoint> entries, std::uint32_t slots, Predicate predicate)
        {
            std::vector<bool> used(slots, false);
            for (const Breakpoint& entry : entries)
            {
                if (entry.slot < slots && predicate(entry))
                {
                    used[entry.slot] = true;
                }
            }
            for (std::uint32_t slot = 0; slot < slots; ++slot)
            {
                if (!used[slot])
                {
                    return slot;
                }
            }
            return std::nullopt;
        }
    }

    std::size_t BreakpointTable::normalize_size(Kind kind, std::size_t size) noexcept
    {
        if (kind == Kind::software || kind == Kind::hardware_execute)
        {
            return 1;
        }
        return size;
    }

    bool BreakpointTable::valid_size(Kind kind, std::size_t size) noexcept
    {
        if (kind == Kind::software || kind == Kind::hardware_execute)
        {
            return true;
        }
        return size == 1 || size == 2 || size == 4 || size == 8;
    }

    std::optional<std::uint32_t> BreakpointTable::free_software_slot() const
    {
        return first_free_slot(entries_,
                               software_slots,
                               [](const Breakpoint& entry)
                               {
                                   return !is_hardware(entry.kind);
                               });
    }

    std::optional<std::uint32_t> BreakpointTable::free_hardware_slot() const
    {
        return first_free_slot(entries_,
                               hardware_slots,
                               [](const Breakpoint& entry)
                               {
                                   return is_hardware(entry.kind);
                               });
    }

    bool BreakpointTable::covered(std::uint64_t address, Kind kind) const
    {
        return std::ranges::any_of(entries_,
                                   [address, kind](const Breakpoint& entry)
                                   {
                                       return entry.address == address && entry.kind == kind;
                                   });
    }

    std::expected<std::uint64_t, std::string>
    BreakpointTable::add(std::string expression, std::uint64_t address, Kind kind, std::size_t size, bool hidden)
    {
        if (!valid_size(kind, size))
        {
            return std::unexpected(std::string {"hardware breakpoint size must be 1, 2, 4 or 8"});
        }
        if (covered(address, kind))
        {
            return std::unexpected(std::string {"a breakpoint already covers this address"});
        }

        std::uint32_t slot = 0;
        if (is_hardware(kind))
        {
            const auto free = free_hardware_slot();
            if (!free)
            {
                return std::unexpected(std::string {"no hardware slot is free"});
            }
            slot = *free;
        }
        else
        {
            const auto free = free_software_slot();
            if (!free)
            {
                return std::unexpected(std::string {"no software slot is free"});
            }
            slot = *free;
        }

        Breakpoint entry;
        entry.id               = next_id_++;
        entry.kind             = kind;
        entry.size             = normalize_size(kind, size);
        entry.expression       = std::move(expression);
        entry.address          = address;
        entry.enabled          = true;
        entry.slot             = slot;
        entry.hidden           = hidden;
        const std::uint64_t id = entry.id;
        entries_.push_back(std::move(entry));
        return id;
    }

    bool BreakpointTable::remove(std::uint64_t id)
    {
        const auto found = std::ranges::find(entries_, id, &Breakpoint::id);
        if (found == entries_.end())
        {
            return false;
        }
        entries_.erase(found);
        return true;
    }

    void BreakpointTable::clear()
    {
        entries_.clear();
    }

    Breakpoint* BreakpointTable::find(std::uint64_t id)
    {
        const auto found = std::ranges::find(entries_, id, &Breakpoint::id);
        return found != entries_.end() ? &*found : nullptr;
    }

    const Breakpoint* BreakpointTable::find(std::uint64_t id) const
    {
        const auto found = std::ranges::find(entries_, id, &Breakpoint::id);
        return found != entries_.end() ? &*found : nullptr;
    }

    const Breakpoint* BreakpointTable::software_at(std::uint64_t address) const
    {
        const auto found = std::ranges::find_if(entries_,
                                                [address](const Breakpoint& entry)
                                                {
                                                    return !is_hardware(entry.kind) && entry.address == address;
                                                });
        return found != entries_.end() ? &*found : nullptr;
    }

    const Breakpoint* BreakpointTable::hardware_in_slot(std::uint32_t slot) const
    {
        const auto found = std::ranges::find_if(entries_,
                                                [slot](const Breakpoint& entry)
                                                {
                                                    return is_hardware(entry.kind) && entry.slot == slot;
                                                });
        return found != entries_.end() ? &*found : nullptr;
    }

    std::span<const Breakpoint> BreakpointTable::entries() const noexcept
    {
        return entries_;
    }

    bool BreakpointTable::empty() const noexcept
    {
        return entries_.empty();
    }

    const Breakpoint* BreakpointTable::count_hit(std::uint64_t id)
    {
        const auto found = std::ranges::find(entries_, id, &Breakpoint::id);
        if (found == entries_.end())
        {
            return nullptr;
        }
        ++found->hits;
        return &*found;
    }

}
