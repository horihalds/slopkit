#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slopkit::ui::components
{
    struct ReferencedString
    {
        std::uint64_t instruction {};
        std::uint64_t string_address {};
        std::string   text;
        bool          wide {};
    };

    class ReferencedStringTable
    {
    public:
        void set_module(std::string module, std::vector<ReferencedString> entries);
        void clear();

        [[nodiscard]] std::span<const ReferencedString> at(std::uint64_t instruction) const;
        [[nodiscard]] std::span<const ReferencedString> module_entries(std::string_view module) const;
        [[nodiscard]] std::vector<ReferencedString>     all_entries() const;
        [[nodiscard]] std::vector<std::string>          modules() const;
        [[nodiscard]] std::size_t                       total() const noexcept;
        [[nodiscard]] bool                              empty() const noexcept;

    private:
        struct ModuleEntries
        {
            std::string                   name;
            std::vector<ReferencedString> entries;
        };

        std::vector<ModuleEntries> modules_;
    };

}
