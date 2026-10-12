#pragma once

#include <cstddef>
#include <string>

namespace slopkit::structs
{
    enum class FieldKind
    {
        integer,
        floating,
        pointer,
        array,
        text,
        structure,
    };

    struct FieldType
    {
        FieldKind   kind {FieldKind::integer};
        std::size_t width {4};
        bool        is_signed {true};
        std::size_t count {1};
        bool        wide {false};
        std::string structure {};

        [[nodiscard]] std::size_t size(std::size_t pointer_size) const noexcept;

        [[nodiscard]] std::string describe() const;

        friend bool operator==(const FieldType&, const FieldType&) = default;
    };

    [[nodiscard]] constexpr bool is_valid_width(std::size_t width) noexcept
    {
        return width == 1 || width == 2 || width == 4 || width == 8;
    }

}
