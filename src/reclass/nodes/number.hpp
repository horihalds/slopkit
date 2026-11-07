#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "reclass/nodes/node.hpp"

namespace slopkit::reclass
{
    using Int8Node   = ScalarNode<NodeKind::int8, 1>;
    using Int16Node  = ScalarNode<NodeKind::int16, 2>;
    using Int32Node  = ScalarNode<NodeKind::int32, 4>;
    using Int64Node  = ScalarNode<NodeKind::int64, 8>;
    using UInt8Node  = ScalarNode<NodeKind::uint8, 1>;
    using UInt16Node = ScalarNode<NodeKind::uint16, 2>;
    using UInt32Node = ScalarNode<NodeKind::uint32, 4>;
    using UInt64Node = ScalarNode<NodeKind::uint64, 8>;
    using FloatNode  = ScalarNode<NodeKind::float32, 4>;
    using DoubleNode = ScalarNode<NodeKind::float64, 8>;
    using BoolNode   = ScalarNode<NodeKind::boolean, 1>;
    using CharNode   = ScalarNode<NodeKind::character, 1>;
    using NIntNode   = ScalarNode<NodeKind::nint, 8>;
    using NUIntNode  = ScalarNode<NodeKind::nuint, 8>;

    // A run of `bits` bits inside a storage unit of the next whole byte count.
    // `signed_bits` sign-extends the value the way the reference's signed
    // bitfield does.
    class BitFieldNode : public Node
    {
    public:
        std::size_t bits {1};
        bool        signed_bits {false};

        [[nodiscard]] NodeKind kind() const noexcept override
        {
            return NodeKind::bitfield;
        }

        [[nodiscard]] std::size_t size(const Project&) const override
        {
            return (bits + 7) / 8;
        }

        [[nodiscard]] std::size_t value_size() const noexcept override
        {
            return (bits + 7) / 8;
        }

        [[nodiscard]] NodePtr clone() const override
        {
            return std::make_unique<BitFieldNode>(*this);
        }

        [[nodiscard]] std::string_view extra_text() const noexcept override;

        // The field's value, sign-extended when the field is signed.
        [[nodiscard]] std::uint64_t read(std::span<const std::byte> bytes) const noexcept;

    private:
        mutable std::string extra_cache_;
    };

    // An enum: a numeric value whose name comes from the project enum
    // `enum_name`. The width is cached so the grid can size its read even when
    // the enum was renamed away.
    class EnumNode : public Node
    {
    public:
        std::string enum_name;
        std::size_t size_bytes {4};

        [[nodiscard]] NodeKind kind() const noexcept override
        {
            return NodeKind::enum_node;
        }

        [[nodiscard]] std::size_t size(const Project&) const override
        {
            return size_bytes;
        }

        [[nodiscard]] std::size_t value_size() const noexcept override
        {
            return size_bytes;
        }

        [[nodiscard]] NodePtr clone() const override
        {
            return std::make_unique<EnumNode>(*this);
        }

        [[nodiscard]] std::string_view extra_text() const noexcept override
        {
            return enum_name;
        }
    };
}
