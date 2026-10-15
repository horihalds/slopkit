#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace slopkit::reclass
{
    enum class NodeKind
    {
        int8,
        int16,
        int32,
        int64,
        uint8,
        uint16,
        uint32,
        uint64,
        float32,
        float64,
        boolean,
        character,
        hex8,
        hex16,
        hex32,
        hex64,
        pointer,
        class_instance,
        class_pointer,
        array,
        union_value,
        class_instance_array,
        class_wrapper_array,
        nint,
        nuint,
        bitfield,
        enum_node,
        utf8_text,
        utf8_text_ptr,
        utf16_text,
        utf16_text_ptr,
        utf32_text,
        utf32_text_ptr,
        vector2,
        vector3,
        vector4,
        matrix3x3,
        matrix3x4,
        matrix4x4,
        virtual_method_table,
        virtual_method,
        function,
        function_ptr,
    };

    // A function node covers a block of code the user can shrink; the
    // reference's default is the one constant to change if that default moves.
    inline constexpr std::size_t kFunctionBytes = 64;

    // The bytes a kind occupies on its own. A class_instance and the class and
    // union arrays are sized by their class or their children, so they report
    // zero here.
    [[nodiscard]] constexpr std::size_t kind_size(NodeKind kind) noexcept
    {
        switch (kind)
        {
        case NodeKind::int8:
        case NodeKind::uint8:
        case NodeKind::boolean:
        case NodeKind::character:
        case NodeKind::hex8:
            return 1;
        case NodeKind::int16:
        case NodeKind::uint16:
        case NodeKind::hex16:
            return 2;
        case NodeKind::int32:
        case NodeKind::uint32:
        case NodeKind::float32:
        case NodeKind::hex32:
            return 4;
        case NodeKind::int64:
        case NodeKind::uint64:
        case NodeKind::float64:
        case NodeKind::hex64:
        case NodeKind::pointer:
        case NodeKind::class_pointer:
            return 8;
        case NodeKind::class_instance_array:
            return 8;
        case NodeKind::nint:
        case NodeKind::nuint:
        case NodeKind::utf8_text_ptr:
        case NodeKind::utf16_text_ptr:
        case NodeKind::utf32_text_ptr:
        case NodeKind::vector2:
        case NodeKind::virtual_method_table:
        case NodeKind::virtual_method:
        case NodeKind::function_ptr:
            return 8;
        case NodeKind::vector3:
            return 12;
        case NodeKind::vector4:
            return 16;
        case NodeKind::matrix3x3:
            return 36;
        case NodeKind::matrix3x4:
            return 48;
        case NodeKind::matrix4x4:
            return 64;
        case NodeKind::function:
            return kFunctionBytes;
        case NodeKind::class_instance:
        case NodeKind::array:
        case NodeKind::union_value:
        case NodeKind::class_wrapper_array:
        case NodeKind::bitfield:
        case NodeKind::enum_node:
        case NodeKind::utf8_text:
        case NodeKind::utf16_text:
        case NodeKind::utf32_text:
            return 0;
        }
        return 0;
    }

    // The stable token the .skt member text and the persisted kind use.
    [[nodiscard]] std::string_view type_token(NodeKind kind) noexcept;

    [[nodiscard]] std::optional<NodeKind> kind_from_token(std::string_view token);

    // The wording the grid's Type column and the node menus show.
    [[nodiscard]] std::string_view describe(NodeKind kind) noexcept;

    [[nodiscard]] bool is_editable(NodeKind kind) noexcept;

    [[nodiscard]] bool is_class_kind(NodeKind kind) noexcept;

    [[nodiscard]] bool is_hex_kind(NodeKind kind) noexcept;

    // The kinds whose own value is an address: the grid draws it in its pointer
    // field and leaves the value cell empty.
    [[nodiscard]] bool is_pointer_kind(NodeKind kind) noexcept;

}
