#include "expr/resolver.hpp"

#include <cctype>
#include <format>

namespace slopkit::expr
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

    std::expected<std::uint64_t, ResolveError> evaluate(const Expression&    expression,
                                                        Modules              modules,
                                                        Symbols              symbols,
                                                        const PointerReader& reader,
                                                        Options /*options*/)
    {
        std::uint64_t address    = 0;
        bool          base_found = false;
        for (const auto& module : modules)
        {
            if (equals_case_insensitive(module.name, expression.base))
            {
                address    = module.base;
                base_found = true;
                break;
            }
        }
        // A symbol resolves after the module map (a loaded image can never be
        // shadowed) and before the literal parse (a symbol wins over the bare-hex
        // reading of a name like `deadbeef`).
        if (!base_found)
        {
            for (const auto& symbol : symbols)
            {
                if (equals_case_insensitive(symbol.name, expression.base))
                {
                    address    = symbol.value;
                    base_found = true;
                    break;
                }
            }
        }
        if (!base_found)
        {
            const auto literal = parse_literal(expression.base);
            if (!literal)
            {
                return std::unexpected(
                    ResolveError {std::format("unknown module or literal '{}'", expression.base), 0});
            }
            address = *literal;
        }

        for (std::size_t i = 0; i < expression.offsets.size(); ++i)
        {
            if (i == 0)
            {
                address += expression.offsets[i].value;
                continue;
            }
            const auto pointer = reader(address);
            if (!pointer)
            {
                return std::unexpected(
                    ResolveError {std::format("cannot read pointer at level {}: {}", i, pointer.error()), i});
            }
            address = *pointer + expression.offsets[i].value;
        }
        return address;
    }

} // namespace slopkit::expr
