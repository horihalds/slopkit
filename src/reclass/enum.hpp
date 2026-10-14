#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace slopkit::reclass
{
    // One named value of an enum.
    struct EnumValue
    {
        std::string   name;
        std::uint64_t value {};
        std::string   comment;
    };

    // The named values an enum node and a bitfield node pick from. `size` is the
    // enum's storage width in bytes and is always one, two, four or eight.
    struct EnumDescription
    {
        std::string            name;
        std::size_t            size {4};
        std::vector<EnumValue> values;
    };

    // The four widths an enum may take, in the order the editor offers them.
    inline constexpr std::size_t kEnumSizes[] {1, 2, 4, 8};

    [[nodiscard]] bool is_enum_size(std::size_t size) noexcept;

    // The name of the value equal to `value`, or an empty view when the enum does
    // not name it.
    [[nodiscard]] std::string_view enum_value_name(const EnumDescription& description, std::uint64_t value) noexcept;

}
