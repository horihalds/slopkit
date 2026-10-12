#include "ui/components/referenced_strings.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slopkit::ui::components
{
    void ReferencedStringTable::set_module(std::string module, std::vector<ReferencedString> entries)
    {
        for (ModuleEntries& existing : modules_)
        {
            if (existing.name == module)
            {
                existing.entries = std::move(entries);
                return;
            }
        }
        modules_.push_back(ModuleEntries {.name = std::move(module), .entries = std::move(entries)});
    }

    void ReferencedStringTable::clear()
    {
        modules_.clear();
    }

    std::span<const ReferencedString> ReferencedStringTable::at(std::uint64_t instruction) const
    {
        for (const ModuleEntries& module : modules_)
        {
            for (std::size_t index = 0; index < module.entries.size(); ++index)
            {
                if (module.entries[index].instruction != instruction)
                {
                    continue;
                }
                std::size_t end = index;
                while (end < module.entries.size() && module.entries[end].instruction == instruction)
                {
                    ++end;
                }
                return std::span<const ReferencedString>(module.entries.data() + index, end - index);
            }
        }
        return {};
    }

    std::span<const ReferencedString> ReferencedStringTable::module_entries(std::string_view module) const
    {
        for (const ModuleEntries& existing : modules_)
        {
            if (existing.name == module)
            {
                return std::span<const ReferencedString>(existing.entries);
            }
        }
        return {};
    }

    std::vector<ReferencedString> ReferencedStringTable::all_entries() const
    {
        std::vector<ReferencedString> entries;
        for (const ModuleEntries& module : modules_)
        {
            entries.insert(entries.end(), module.entries.begin(), module.entries.end());
        }
        return entries;
    }

    std::vector<std::string> ReferencedStringTable::modules() const
    {
        std::vector<std::string> names;
        names.reserve(modules_.size());
        for (const ModuleEntries& module : modules_)
        {
            names.push_back(module.name);
        }
        return names;
    }

    std::size_t ReferencedStringTable::total() const noexcept
    {
        std::size_t total = 0;
        for (const ModuleEntries& module : modules_)
        {
            total += module.entries.size();
        }
        return total;
    }

    bool ReferencedStringTable::empty() const noexcept
    {
        return total() == 0;
    }

}
