#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>

#include "expr/expression.hpp"

namespace slopkit::expr
{

    // One known module image: the name the views display and its load base.
    struct ModuleRef
    {
        std::string   name;
        std::uint64_t base {};
    };

    using Modules = std::span<const ModuleRef>;

    // One script-registered name and its 64-bit value. A symbol resolves like a
    // module name does, after the module lookup and before the literal parse;
    // a `0` value is a value, not an "unset" marker.
    struct SymbolRef
    {
        std::string   name;
        std::uint64_t value {};
    };

    using Symbols = std::span<const SymbolRef>;

    // Reads the little-endian pointer stored at `address`; the error text is the
    // caller's reason. The closure owns the pointer width, so the evaluator never
    // needs to know it.
    using PointerReader = std::function<std::expected<std::uint64_t, std::string>(std::uint64_t address)>;

    struct Options
    {
        std::size_t pointer_size {8};
    };

    struct ResolveError
    {
        std::string message;
        std::size_t failed_level {}; // 1-based pointer level; 0 when the base failed
    };

    // Resolves `expression` to an absolute address. The base is looked up as a
    // module name first (case-insensitive), then as a symbol name
    // (case-insensitive), then parsed as a literal, and each offset after the
    // first dereferences first:
    //   address = base + o1; address = reader(address) + oi (for every later oi).
    [[nodiscard]] std::expected<std::uint64_t, ResolveError> evaluate(const Expression&    expression,
                                                                      Modules              modules,
                                                                      Symbols              symbols,
                                                                      const PointerReader& reader,
                                                                      Options              options = {});

} // namespace slopkit::expr
