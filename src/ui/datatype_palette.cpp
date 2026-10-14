#include "ui/datatype_palette.hpp"

#include "ui/theme.hpp"

namespace slopkit::ui
{
    std::string_view datatype_icon_name(reclass::NodeKind kind) noexcept
    {
        switch (kind)
        {
        case reclass::NodeKind::int8:
            return "int8";
        case reclass::NodeKind::int16:
            return "int16";
        case reclass::NodeKind::int32:
            return "int32";
        case reclass::NodeKind::int64:
            return "int64";
        case reclass::NodeKind::uint8:
            return "uint8";
        case reclass::NodeKind::uint16:
            return "uint16";
        case reclass::NodeKind::uint32:
            return "uint32";
        case reclass::NodeKind::uint64:
            return "uint64";
        case reclass::NodeKind::float32:
            return "float";
        case reclass::NodeKind::float64:
            return "double";
        case reclass::NodeKind::boolean:
            return "bool";
        case reclass::NodeKind::hex8:
            return "hex8";
        case reclass::NodeKind::hex16:
            return "hex16";
        case reclass::NodeKind::hex32:
            return "hex32";
        case reclass::NodeKind::hex64:
            return "hex64";
        case reclass::NodeKind::pointer:
            return "pointer";
        case reclass::NodeKind::array:
            return "array";
        case reclass::NodeKind::bitfield:
            return "bits";
        case reclass::NodeKind::enum_node:
            return "enumeration";
        case reclass::NodeKind::utf8_text:
            return "utf8text";
        case reclass::NodeKind::utf16_text:
            return "utf16text";
        case reclass::NodeKind::utf32_text:
            return "utf32text";
        case reclass::NodeKind::utf8_text_ptr:
        case reclass::NodeKind::utf16_text_ptr:
        case reclass::NodeKind::utf32_text_ptr:
            return "pointer_to_text";
        case reclass::NodeKind::vector2:
            return "vector2";
        case reclass::NodeKind::vector3:
            return "vector3";
        case reclass::NodeKind::vector4:
            return "vector4";
        case reclass::NodeKind::matrix3x3:
            return "matrix3x3";
        case reclass::NodeKind::matrix3x4:
            return "matrix3x4";
        case reclass::NodeKind::matrix4x4:
            return "matrix4x4";
        case reclass::NodeKind::character:
        case reclass::NodeKind::class_instance:
        case reclass::NodeKind::class_pointer:
        case reclass::NodeKind::union_value:
        case reclass::NodeKind::class_instance_array:
        case reclass::NodeKind::class_wrapper_array:
        case reclass::NodeKind::nint:
        case reclass::NodeKind::nuint:
        case reclass::NodeKind::virtual_method_table:
        case reclass::NodeKind::virtual_method:
        case reclass::NodeKind::function:
        case reclass::NodeKind::function_ptr:
            return {};
        }
        return {};
    }

    QColor datatype_colour(reclass::NodeKind kind)
    {
        const Theme& theme = active_theme();
        switch (kind)
        {
        case reclass::NodeKind::nint:
        case reclass::NodeKind::int8:
        case reclass::NodeKind::int16:
        case reclass::NodeKind::int32:
        case reclass::NodeKind::int64:
            return theme.datatype_signed;
        case reclass::NodeKind::nuint:
        case reclass::NodeKind::uint8:
        case reclass::NodeKind::uint16:
        case reclass::NodeKind::uint32:
        case reclass::NodeKind::uint64:
            return theme.datatype_unsigned;
        case reclass::NodeKind::bitfield:
            return theme.datatype_bits;
        case reclass::NodeKind::float32:
        case reclass::NodeKind::float64:
        case reclass::NodeKind::boolean:
            return theme.datatype_float;
        case reclass::NodeKind::vector2:
        case reclass::NodeKind::vector3:
        case reclass::NodeKind::vector4:
        case reclass::NodeKind::matrix3x3:
        case reclass::NodeKind::matrix3x4:
        case reclass::NodeKind::matrix4x4:
            return theme.datatype_vector;
        case reclass::NodeKind::utf8_text:
        case reclass::NodeKind::utf8_text_ptr:
        case reclass::NodeKind::utf16_text:
        case reclass::NodeKind::utf16_text_ptr:
        case reclass::NodeKind::utf32_text:
        case reclass::NodeKind::utf32_text_ptr:
            return theme.datatype_text;
        case reclass::NodeKind::class_instance:
        case reclass::NodeKind::class_pointer:
            return theme.datatype_class;
        case reclass::NodeKind::character:
        case reclass::NodeKind::hex8:
        case reclass::NodeKind::hex16:
        case reclass::NodeKind::hex32:
        case reclass::NodeKind::hex64:
        case reclass::NodeKind::pointer:
        case reclass::NodeKind::array:
        case reclass::NodeKind::union_value:
        case reclass::NodeKind::class_instance_array:
        case reclass::NodeKind::class_wrapper_array:
        case reclass::NodeKind::enum_node:
        case reclass::NodeKind::virtual_method_table:
        case reclass::NodeKind::virtual_method:
        case reclass::NodeKind::function:
        case reclass::NodeKind::function_ptr:
            break;
        }
        return theme.text;
    }
}
