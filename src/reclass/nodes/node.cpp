#include "reclass/nodes/node.hpp"

#include <format>
#include <string>

#include "reclass/nodes/container.hpp"
#include "reclass/nodes/function.hpp"
#include "reclass/nodes/hex.hpp"
#include "reclass/nodes/number.hpp"
#include "reclass/nodes/text.hpp"
#include "reclass/nodes/vector.hpp"

namespace slopkit::reclass
{
    std::string count_label(std::size_t count)
    {
        return std::format("[{}]", count);
    }

    NodePtr make_node(NodeKind kind)
    {
        switch (kind)
        {
        case NodeKind::int8:
            return std::make_unique<Int8Node>();
        case NodeKind::int16:
            return std::make_unique<Int16Node>();
        case NodeKind::int32:
            return std::make_unique<Int32Node>();
        case NodeKind::int64:
            return std::make_unique<Int64Node>();
        case NodeKind::uint8:
            return std::make_unique<UInt8Node>();
        case NodeKind::uint16:
            return std::make_unique<UInt16Node>();
        case NodeKind::uint32:
            return std::make_unique<UInt32Node>();
        case NodeKind::uint64:
            return std::make_unique<UInt64Node>();
        case NodeKind::float32:
            return std::make_unique<FloatNode>();
        case NodeKind::float64:
            return std::make_unique<DoubleNode>();
        case NodeKind::boolean:
            return std::make_unique<BoolNode>();
        case NodeKind::character:
            return std::make_unique<CharNode>();
        case NodeKind::hex8:
            return std::make_unique<Hex8Node>();
        case NodeKind::hex16:
            return std::make_unique<Hex16Node>();
        case NodeKind::hex32:
            return std::make_unique<Hex32Node>();
        case NodeKind::hex64:
            return std::make_unique<Hex64Node>();
        case NodeKind::pointer:
            return std::make_unique<PointerNode>();
        case NodeKind::class_pointer:
            return std::make_unique<ClassPointerNode>();
        case NodeKind::class_instance:
            return std::make_unique<ClassInstanceNode>();
        case NodeKind::array:
        {
            auto node = std::make_unique<ArrayNode>();
            node->children_mutable().push_back(make_node(NodeKind::int8));
            return node;
        }
        case NodeKind::union_value:
            return std::make_unique<UnionNode>();
        case NodeKind::class_instance_array:
            return std::make_unique<ClassInstanceArrayNode>();
        case NodeKind::class_wrapper_array:
            return std::make_unique<ClassWrapperArrayNode>();
        case NodeKind::nint:
            return std::make_unique<NIntNode>();
        case NodeKind::nuint:
            return std::make_unique<NUIntNode>();
        case NodeKind::bitfield:
            return std::make_unique<BitFieldNode>();
        case NodeKind::enum_node:
            return std::make_unique<EnumNode>();
        case NodeKind::utf8_text:
            return std::make_unique<UTF8TextNode>();
        case NodeKind::utf8_text_ptr:
            return std::make_unique<UTF8TextPtrNode>();
        case NodeKind::utf16_text:
            return std::make_unique<UTF16TextNode>();
        case NodeKind::utf16_text_ptr:
            return std::make_unique<UTF16TextPtrNode>();
        case NodeKind::utf32_text:
            return std::make_unique<UTF32TextNode>();
        case NodeKind::utf32_text_ptr:
            return std::make_unique<UTF32TextPtrNode>();
        case NodeKind::vector2:
            return std::make_unique<Vector2Node>();
        case NodeKind::vector3:
            return std::make_unique<Vector3Node>();
        case NodeKind::vector4:
            return std::make_unique<Vector4Node>();
        case NodeKind::matrix3x3:
            return std::make_unique<Matrix3x3Node>();
        case NodeKind::matrix3x4:
            return std::make_unique<Matrix3x4Node>();
        case NodeKind::matrix4x4:
            return std::make_unique<Matrix4x4Node>();
        case NodeKind::virtual_method_table:
            return std::make_unique<VirtualMethodTableNode>();
        case NodeKind::virtual_method:
            return std::make_unique<VirtualMethodNode>();
        case NodeKind::function:
            return std::make_unique<FunctionNode>();
        case NodeKind::function_ptr:
            return std::make_unique<FunctionPtrNode>();
        }
        return std::make_unique<Int32Node>();
    }
}
