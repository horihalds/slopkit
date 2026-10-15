#include "reclass/node.hpp"

namespace slopkit::reclass
{
    std::string_view type_token(NodeKind kind) noexcept
    {
        switch (kind)
        {
        case NodeKind::int8:
            return "i8";
        case NodeKind::int16:
            return "i16";
        case NodeKind::int32:
            return "i32";
        case NodeKind::int64:
            return "i64";
        case NodeKind::uint8:
            return "u8";
        case NodeKind::uint16:
            return "u16";
        case NodeKind::uint32:
            return "u32";
        case NodeKind::uint64:
            return "u64";
        case NodeKind::float32:
            return "f32";
        case NodeKind::float64:
            return "f64";
        case NodeKind::boolean:
            return "bool";
        case NodeKind::character:
            return "char";
        case NodeKind::hex8:
            return "hex8";
        case NodeKind::hex16:
            return "hex16";
        case NodeKind::hex32:
            return "hex32";
        case NodeKind::hex64:
            return "hex64";
        case NodeKind::pointer:
            return "pointer";
        case NodeKind::class_instance:
            return "class_instance";
        case NodeKind::class_pointer:
            return "class_pointer";
        case NodeKind::array:
            return "array";
        case NodeKind::union_value:
            return "union";
        case NodeKind::class_instance_array:
            return "class_instance_array";
        case NodeKind::class_wrapper_array:
            return "class_wrapper_array";
        case NodeKind::nint:
            return "nint";
        case NodeKind::nuint:
            return "nuint";
        case NodeKind::bitfield:
            return "bitfield";
        case NodeKind::enum_node:
            return "enum";
        case NodeKind::utf8_text:
            return "utf8_text";
        case NodeKind::utf8_text_ptr:
            return "utf8_text_ptr";
        case NodeKind::utf16_text:
            return "utf16_text";
        case NodeKind::utf16_text_ptr:
            return "utf16_text_ptr";
        case NodeKind::utf32_text:
            return "utf32_text";
        case NodeKind::utf32_text_ptr:
            return "utf32_text_ptr";
        case NodeKind::vector2:
            return "vector2";
        case NodeKind::vector3:
            return "vector3";
        case NodeKind::vector4:
            return "vector4";
        case NodeKind::matrix3x3:
            return "matrix3x3";
        case NodeKind::matrix3x4:
            return "matrix3x4";
        case NodeKind::matrix4x4:
            return "matrix4x4";
        case NodeKind::virtual_method_table:
            return "virtual_method_table";
        case NodeKind::virtual_method:
            return "virtual_method";
        case NodeKind::function:
            return "function";
        case NodeKind::function_ptr:
            return "function_ptr";
        }
        return {};
    }

    std::optional<NodeKind> kind_from_token(std::string_view token)
    {
        for (const NodeKind kind : {NodeKind::int8,
                                    NodeKind::int16,
                                    NodeKind::int32,
                                    NodeKind::int64,
                                    NodeKind::uint8,
                                    NodeKind::uint16,
                                    NodeKind::uint32,
                                    NodeKind::uint64,
                                    NodeKind::float32,
                                    NodeKind::float64,
                                    NodeKind::boolean,
                                    NodeKind::character,
                                    NodeKind::hex8,
                                    NodeKind::hex16,
                                    NodeKind::hex32,
                                    NodeKind::hex64,
                                    NodeKind::pointer,
                                    NodeKind::class_instance,
                                    NodeKind::class_pointer,
                                    NodeKind::array,
                                    NodeKind::union_value,
                                    NodeKind::class_instance_array,
                                    NodeKind::class_wrapper_array,
                                    NodeKind::nint,
                                    NodeKind::nuint,
                                    NodeKind::bitfield,
                                    NodeKind::enum_node,
                                    NodeKind::utf8_text,
                                    NodeKind::utf8_text_ptr,
                                    NodeKind::utf16_text,
                                    NodeKind::utf16_text_ptr,
                                    NodeKind::utf32_text,
                                    NodeKind::utf32_text_ptr,
                                    NodeKind::vector2,
                                    NodeKind::vector3,
                                    NodeKind::vector4,
                                    NodeKind::matrix3x3,
                                    NodeKind::matrix3x4,
                                    NodeKind::matrix4x4,
                                    NodeKind::virtual_method_table,
                                    NodeKind::virtual_method,
                                    NodeKind::function,
                                    NodeKind::function_ptr})
        {
            if (type_token(kind) == token)
            {
                return kind;
            }
        }
        return std::nullopt;
    }

    std::string_view describe(NodeKind kind) noexcept
    {
        switch (kind)
        {
        case NodeKind::int8:
            return "Int8";
        case NodeKind::int16:
            return "Int16";
        case NodeKind::int32:
            return "Int32";
        case NodeKind::int64:
            return "Int64";
        case NodeKind::uint8:
            return "UInt8";
        case NodeKind::uint16:
            return "UInt16";
        case NodeKind::uint32:
            return "UInt32";
        case NodeKind::uint64:
            return "UInt64";
        case NodeKind::float32:
            return "Float";
        case NodeKind::float64:
            return "Double";
        case NodeKind::boolean:
            return "Bool";
        case NodeKind::character:
            return "Char";
        case NodeKind::hex8:
            return "Hex8";
        case NodeKind::hex16:
            return "Hex16";
        case NodeKind::hex32:
            return "Hex32";
        case NodeKind::hex64:
            return "Hex64";
        case NodeKind::pointer:
            return "Pointer";
        case NodeKind::class_instance:
            return "Instance";
        case NodeKind::class_pointer:
            return "Class Pointer";
        case NodeKind::array:
            return "Array";
        case NodeKind::union_value:
            return "Union";
        case NodeKind::class_instance_array:
            return "Class Instance Array";
        case NodeKind::class_wrapper_array:
            return "Class Array";
        case NodeKind::nint:
            return "NInt";
        case NodeKind::nuint:
            return "NUInt";
        case NodeKind::bitfield:
            return "BitField";
        case NodeKind::enum_node:
            return "Enum";
        case NodeKind::utf8_text:
            return "Utf8Text";
        case NodeKind::utf8_text_ptr:
            return "Utf8TextPtr";
        case NodeKind::utf16_text:
            return "Utf16Text";
        case NodeKind::utf16_text_ptr:
            return "Utf16TextPtr";
        case NodeKind::utf32_text:
            return "Utf32Text";
        case NodeKind::utf32_text_ptr:
            return "Utf32TextPtr";
        case NodeKind::vector2:
            return "Vector2";
        case NodeKind::vector3:
            return "Vector3";
        case NodeKind::vector4:
            return "Vector4";
        case NodeKind::matrix3x3:
            return "Matrix3x3";
        case NodeKind::matrix3x4:
            return "Matrix3x4";
        case NodeKind::matrix4x4:
            return "Matrix4x4";
        case NodeKind::virtual_method_table:
            return "Virtual Method Table";
        case NodeKind::virtual_method:
            return "Virtual Method";
        case NodeKind::function:
            return "Function";
        case NodeKind::function_ptr:
            return "Function Ptr";
        }
        return {};
    }

    bool is_editable(NodeKind kind) noexcept
    {
        switch (kind)
        {
        case NodeKind::hex8:
        case NodeKind::hex16:
        case NodeKind::hex32:
        case NodeKind::hex64:
        case NodeKind::pointer:
        case NodeKind::class_pointer:
        case NodeKind::class_instance:
        case NodeKind::class_instance_array:
        case NodeKind::class_wrapper_array:
        case NodeKind::array:
        case NodeKind::union_value:
        case NodeKind::virtual_method_table:
        case NodeKind::virtual_method:
        case NodeKind::function:
        case NodeKind::function_ptr:
        case NodeKind::utf8_text_ptr:
        case NodeKind::utf16_text_ptr:
        case NodeKind::utf32_text_ptr:
        case NodeKind::bitfield:
        case NodeKind::enum_node:
        case NodeKind::utf8_text:
        case NodeKind::utf16_text:
        case NodeKind::utf32_text:
        case NodeKind::vector2:
        case NodeKind::vector3:
        case NodeKind::vector4:
        case NodeKind::matrix3x3:
        case NodeKind::matrix3x4:
        case NodeKind::matrix4x4:
            return false;
        default:
            return true;
        }
    }

    bool is_class_kind(NodeKind kind) noexcept
    {
        return kind == NodeKind::class_instance || kind == NodeKind::class_pointer
            || kind == NodeKind::class_instance_array || kind == NodeKind::class_wrapper_array;
    }

    bool is_hex_kind(NodeKind kind) noexcept
    {
        return kind == NodeKind::hex8 || kind == NodeKind::hex16 || kind == NodeKind::hex32 || kind == NodeKind::hex64;
    }

    bool is_pointer_kind(NodeKind kind) noexcept
    {
        return kind == NodeKind::pointer || kind == NodeKind::class_pointer || kind == NodeKind::class_instance_array;
    }

}
