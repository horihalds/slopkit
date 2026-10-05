#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace slopkit::expr
{

    // One additive offset; every offset except the last separates a dereference.
    struct Offset
    {
        std::uint64_t value {};
    };

    // A parsed target expression: the base token (a module name or a numeric
    // literal) plus the ordered additive offsets. A module name always wins over
    // a literal-looking base, so a module literally named like a hex run still
    // resolves as that module.
    struct Expression
    {
        std::string         base; // module name or numeric literal text
        std::vector<Offset> offsets;

        // Pointer dereferences the expression performs (offsets - 1, floored at 0).
        [[nodiscard]] std::size_t pointer_levels() const noexcept;
    };

    struct Error
    {
        std::string message;
    };

    // Parses one numeric literal. `0x`/`0X`-prefixed and bare tokens are
    // hexadecimal, a leading `#` marks a decimal value; empty tokens and trailing
    // garbage are rejected.
    [[nodiscard]] std::expected<std::uint64_t, Error> parse_literal(std::string_view text);

    // Splits `base ( "+" offset )*`, trimming whitespace around every token. The
    // base is kept as text for the evaluator (a module name is looked up first, a
    // literal second) and only its emptiness is checked here; every offset is
    // validated. The base is empty, an offset is missing or an offset is not a
    // literal are rejected.
    [[nodiscard]] std::expected<Expression, Error> parse(std::string_view text);

} // namespace slopkit::expr
