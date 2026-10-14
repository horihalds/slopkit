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
                                    NodeKind::class_pointer})
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
        }
        return {};
    }

    bool is_editable(NodeKind kind) noexcept
    {
        return kind_size(kind) != 0 && !is_hex_kind(kind) && kind != NodeKind::pointer
            && kind != NodeKind::class_pointer;
    }

    bool is_class_kind(NodeKind kind) noexcept
    {
        return kind == NodeKind::class_instance || kind == NodeKind::class_pointer;
    }

    bool is_hex_kind(NodeKind kind) noexcept
    {
        return kind == NodeKind::hex8 || kind == NodeKind::hex16 || kind == NodeKind::hex32 || kind == NodeKind::hex64;
    }

}
