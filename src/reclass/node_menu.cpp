#include "reclass/node_menu.hpp"

namespace slopkit::reclass
{
    namespace
    {
        // The reference's NodeTypesBuilder order: hex first, then the signed
        // integers, the unsigned integers, the bit kinds, the floats, the
        // vectors and matrices, the text kinds, the reference kinds and finally
        // the code kinds.
        const std::vector<NodeMenuGroup> kGroups {
            {      "Hex",
             {{NodeKind::hex64, "Hex64"},
             {NodeKind::hex32, "Hex32"},
             {NodeKind::hex16, "Hex16"},
             {NodeKind::hex8, "Hex8"}}                                                                                  },
            {  "Integer",
             {{NodeKind::nint, "NInt"},
             {NodeKind::int64, "Int64"},
             {NodeKind::int32, "Int32"},
             {NodeKind::int16, "Int16"},
             {NodeKind::int8, "Int8"}}                                                                                  },
            { "Unsigned",
             {{NodeKind::nuint, "NUInt"},
             {NodeKind::uint64, "UInt64"},
             {NodeKind::uint32, "UInt32"},
             {NodeKind::uint16, "UInt16"},
             {NodeKind::uint8, "UInt8"}}                                                                                },
            {     "Bits", {{NodeKind::boolean, "Bool"}, {NodeKind::bitfield, "BitField"}, {NodeKind::enum_node, "Enum"}}},
            {    "Float",                                  {{NodeKind::float32, "Float"}, {NodeKind::float64, "Double"}}},
            {   "Vector",
             {{NodeKind::vector4, "Vector4"},
             {NodeKind::vector3, "Vector3"},
             {NodeKind::vector2, "Vector2"},
             {NodeKind::matrix4x4, "Matrix4x4"},
             {NodeKind::matrix3x4, "Matrix3x4"},
             {NodeKind::matrix3x3, "Matrix3x3"}}                                                                        },
            {     "Text",
             {{NodeKind::utf8_text, "Utf8Text"},
             {NodeKind::utf8_text_ptr, "Utf8TextPtr"},
             {NodeKind::utf16_text, "Utf16Text"},
             {NodeKind::utf16_text_ptr, "Utf16TextPtr"}}                                                                },
            {"Reference",
             {{NodeKind::pointer, "Pointer"}, {NodeKind::array, "Array"}, {NodeKind::union_value, "Union"}}             },
            {    "Class",                                                 {{NodeKind::class_instance, "Class Instance"}}},
            {     "Code",
             {{NodeKind::virtual_method_table, "Virtual Method Table"},
             {NodeKind::function, "Function"},
             {NodeKind::function_ptr, "Function Ptr"}}                                                                  },
        };
    }

    std::span<const NodeMenuGroup> node_menu_groups()
    {
        return kGroups;
    }

    std::string_view node_menu_label(NodeKind kind) noexcept
    {
        for (const NodeMenuGroup& group : kGroups)
        {
            for (const NodeMenuEntry& entry : group.entries)
            {
                if (entry.kind == kind)
                {
                    return entry.label;
                }
            }
        }
        return describe(kind);
    }
}
