#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace slopkit::expr
{
    struct Offset
    {
        std::uint64_t value {};
        std::size_t   dereferences {};
    };

    struct Expression
    {
        std::string         base;
        std::vector<Offset> offsets;

        [[nodiscard]] std::size_t pointer_levels() const noexcept;
    };

    struct Error
    {
        std::string message;
    };

    [[nodiscard]] std::expected<std::uint64_t, Error> parse_literal(std::string_view text);

    [[nodiscard]] std::expected<Expression, Error> parse(std::string_view text);

}
