#include "script/symbols.hpp"

#include <algorithm>
#include <cctype>

namespace slopkit::script
{
    namespace
    {
        bool equals_case_insensitive(std::string_view left, std::string_view right)
        {
            if (left.size() != right.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < left.size(); ++i)
            {
                const auto lower = [](char value)
                {
                    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
                };
                if (lower(left[i]) != lower(right[i]))
                {
                    return false;
                }
            }
            return true;
        }

    } // namespace

    // The parser could never produce a base with a `+` (it splits on one) or a
    // leading `#` (that marks a decimal literal), and a blank name cannot be
    // typed; those are rejected at registration rather than stored as an
    // unreachable entry.
    std::optional<std::string> validate_symbol_name(std::string_view name)
    {
        if (name.empty())
        {
            return std::string {"the name must not be empty"};
        }
        if (name.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos)
        {
            return std::string {"the name must not be blank"};
        }
        if (name.find('+') != std::string_view::npos)
        {
            return std::string {"the name must not contain '+'"};
        }
        if (name.front() == '#')
        {
            return std::string {"the name must not start with '#'"};
        }
        return std::nullopt;
    }

    std::expected<void, std::string> SymbolTable::set(std::string_view name, std::uint64_t value)
    {
        if (const std::optional<std::string> error = validate_symbol_name(name))
        {
            return std::unexpected(*error);
        }

        const std::lock_guard lock(mutex_);
        const auto            existing = std::find_if(symbols_.begin(),
                                                      symbols_.end(),
                                                      [name](const expr::SymbolRef& symbol)
                                                      {
                                               return equals_case_insensitive(symbol.name, name);
                                                      });
        if (existing != symbols_.end())
        {
            existing->value = value; // the first spelling is kept
            return {};
        }
        symbols_.push_back(expr::SymbolRef {std::string(name), value});
        return {};
    }

    bool SymbolTable::remove(std::string_view name)
    {
        const std::lock_guard lock(mutex_);
        const auto            existing = std::find_if(symbols_.begin(),
                                                      symbols_.end(),
                                                      [name](const expr::SymbolRef& symbol)
                                                      {
                                               return equals_case_insensitive(symbol.name, name);
                                                      });
        if (existing == symbols_.end())
        {
            return false;
        }
        symbols_.erase(existing);
        return true;
    }

    std::optional<std::uint64_t> SymbolTable::lookup(std::string_view name) const
    {
        const std::lock_guard lock(mutex_);
        for (const expr::SymbolRef& symbol : symbols_)
        {
            if (equals_case_insensitive(symbol.name, name))
            {
                return symbol.value;
            }
        }
        return std::nullopt;
    }

    std::vector<expr::SymbolRef> SymbolTable::snapshot() const
    {
        const std::lock_guard lock(mutex_);
        return symbols_;
    }

    void SymbolTable::clear()
    {
        const std::lock_guard lock(mutex_);
        symbols_.clear();
    }

    std::size_t SymbolTable::size() const
    {
        const std::lock_guard lock(mutex_);
        return symbols_.size();
    }

    SymbolApi SymbolTable::api()
    {
        return SymbolApi {
            .set =
                [this](std::string_view name, std::uint64_t value)
            {
                return set(name, value);
            },
            .remove = [this](std::string_view name) -> std::expected<void, std::string>
            {
                remove(name);
                return {};
            },
            .lookup = [this](std::string_view name) -> std::optional<std::uint64_t>
            {
                return lookup(name);
            },
        };
    }

    SymbolTable& default_symbol_table()
    {
        static SymbolTable table;
        return table;
    }

} // namespace slopkit::script
