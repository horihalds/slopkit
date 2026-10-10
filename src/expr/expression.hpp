#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace slopkit::expr
{

    // One additive offset; `dereferences` is the number of pointer reads
    // performed right after the offset is added.
    struct Offset
    {
        std::uint64_t value {};
        std::size_t   dereferences {};
    };

    // A parsed target expression: the base token (a module name or a numeric
    // literal) plus the ordered additive offsets. A module name always wins over
    // a literal-looking base, so a module literally named like a hex run still
    // resolves as that module.
    struct Expression
    {
        std::string         base; // module name or numeric literal text
        std::vector<Offset> offsets;

        // Pointer dereferences the expression performs (the sum of every
        // offset's dereference count).
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

    // Parses a chain `operand ( "+" offset )*` where `operand` is a base token or
    // a `[` chain `]` group that closes with one pointer dereference; nesting is
    // the chain. Whitespace around every token is trimmed. A bracket-free
    // expression keeps the legacy rule (one dereference between consecutive
    // offsets). The base is kept as text for the evaluator (a module name is
    // looked up first, a literal second); an empty base, a missing or invalid
    // offset, an unbalanced or empty group and more than 32 nested groups are
    // rejected.
    [[nodiscard]] std::expected<Expression, Error> parse(std::string_view text);

} // namespace slopkit::expr
