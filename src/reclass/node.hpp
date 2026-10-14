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
    };

    struct Node
    {
        std::size_t id {};
        NodeKind    kind {NodeKind::int32};
        std::string name;
        std::string comment;
        std::string class_name;
        bool        hex {};
    };

    // The bytes a kind occupies on its own. A class_instance is sized by the
    // class it points at, so it reports zero here.
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
        case NodeKind::class_instance:
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

    [[nodiscard]] std::string default_node_name(NodeKind kind, std::size_t offset);

}
