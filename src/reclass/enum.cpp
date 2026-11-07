#include "reclass/enum.hpp"

#include <algorithm>

namespace slopkit::reclass
{
    bool is_enum_size(std::size_t size) noexcept
    {
        return std::ranges::find(kEnumSizes, size) != std::end(kEnumSizes);
    }

    std::string_view enum_value_name(const EnumDescription& description, std::uint64_t value) noexcept
    {
        const auto found = std::ranges::find_if(description.values,
                                                [value](const EnumValue& entry)
                                                {
                                                    return entry.value == value;
                                                });
        if (found == description.values.end())
        {
            return {};
        }
        return found->name;
    }

}
