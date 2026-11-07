#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "expr/resolver.hpp"
#include "script/types.hpp"

namespace slopkit::script
{
    [[nodiscard]] std::optional<std::string> validate_symbol_name(std::string_view name);

    class SymbolTable
    {
    public:
        std::expected<void, std::string> set(std::string_view name, std::uint64_t value);

        bool remove(std::string_view name);

        [[nodiscard]] std::optional<std::uint64_t> lookup(std::string_view name) const;

        [[nodiscard]] std::vector<expr::SymbolRef> snapshot() const;

        void clear();

        [[nodiscard]] std::size_t size() const;

        [[nodiscard]] SymbolApi api();

    private:
        mutable std::mutex           mutex_;
        std::vector<expr::SymbolRef> symbols_;
    };

    [[nodiscard]] SymbolTable& default_symbol_table();

}
