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
    struct ModuleRef
    {
        std::string   name;
        std::uint64_t base {};
    };

    using Modules = std::span<const ModuleRef>;

    struct SymbolRef
    {
        std::string   name;
        std::uint64_t value {};
    };

    using Symbols = std::span<const SymbolRef>;

    using PointerReader = std::function<std::expected<std::uint64_t, std::string>(std::uint64_t address)>;

    using AddressValidator = std::function<std::expected<bool, std::string>(std::uint64_t address, std::size_t size)>;

    struct Options
    {
        std::size_t pointer_size {8};
    };

    struct ResolveError
    {
        std::string message;
        std::size_t failed_level {};
    };

    [[nodiscard]] std::expected<std::uint64_t, ResolveError> evaluate(const Expression&       expression,
                                                                      Modules                 modules,
                                                                      Symbols                 symbols,
                                                                      const PointerReader&    reader,
                                                                      Options                 options  = {},
                                                                      const AddressValidator& validate = {});

}
