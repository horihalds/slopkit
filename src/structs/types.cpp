#include "structs/types.hpp"

#include <format>

namespace slopkit::structs
{
    std::size_t FieldType::size(std::size_t pointer_size) const noexcept
    {
        switch (kind)
        {
        case FieldKind::integer:
        case FieldKind::floating:
        case FieldKind::array:
            return width * count;
        case FieldKind::pointer:
            return pointer_size * count;
        case FieldKind::text:
            return count * (wide ? 2 : 1);
        case FieldKind::structure:
            // The size of an embedded struct is only known to Structs, which owns
            // the referenced definition, so it is added by Structs::struct_size.
            return 0;
        }
        return 0;
    }

    std::string FieldType::describe() const
    {
        const std::string suffix = count > 1 ? std::format("[{}]", count) : std::string {};
        switch (kind)
        {
        case FieldKind::integer:
            return std::format("{}{}{}", is_signed ? "int" : "uint", width * 8, suffix);
        case FieldKind::floating:
            return std::format("{}{}", width == 8 ? "double" : "float", suffix);
        case FieldKind::pointer:
            return std::format("{}{}", structure.empty() ? std::string {"void*"} : structure + "*", suffix);
        case FieldKind::array:
            return std::format("{}{}{}", is_signed ? "int" : "uint", width * 8, suffix);
        case FieldKind::text:
            return std::format("{}[{}]", wide ? "char16_t" : "char", count);
        case FieldKind::structure:
            return std::format("{}{}", structure.empty() ? std::string {"struct"} : structure, suffix);
        }
        return "?";
    }

}
