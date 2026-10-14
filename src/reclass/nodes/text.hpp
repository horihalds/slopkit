#pragma once

#include <cstddef>
#include <memory>

#include "reclass/nodes/node.hpp"

namespace slopkit::reclass
{
    // A string read from the target: `length` characters of a fixed character
    // width, where the width is what tells the encodings apart.
    class UTF8TextNode : public Node
    {
    public:
        std::size_t length {16};

        [[nodiscard]] NodeKind kind() const noexcept override
        {
            return NodeKind::utf8_text;
        }

        [[nodiscard]] std::size_t size(const Project&) const override
        {
            return length;
        }

        [[nodiscard]] std::size_t value_size() const noexcept override
        {
            return length;
        }

        [[nodiscard]] NodePtr clone() const override
        {
            return std::make_unique<UTF8TextNode>(*this);
        }
    };

    class UTF16TextNode : public Node
    {
    public:
        std::size_t length {16};

        [[nodiscard]] NodeKind kind() const noexcept override
        {
            return NodeKind::utf16_text;
        }

        [[nodiscard]] std::size_t size(const Project&) const override
        {
            return length * 2;
        }

        [[nodiscard]] std::size_t value_size() const noexcept override
        {
            return length * 2;
        }

        [[nodiscard]] NodePtr clone() const override
        {
            return std::make_unique<UTF16TextNode>(*this);
        }
    };

    class UTF32TextNode : public Node
    {
    public:
        std::size_t length {16};

        [[nodiscard]] NodeKind kind() const noexcept override
        {
            return NodeKind::utf32_text;
        }

        [[nodiscard]] std::size_t size(const Project&) const override
        {
            return length * 4;
        }

        [[nodiscard]] std::size_t value_size() const noexcept override
        {
            return length * 4;
        }

        [[nodiscard]] NodePtr clone() const override
        {
            return std::make_unique<UTF32TextNode>(*this);
        }
    };

    // The pointer forms hold the address of the string; the grid shows that
    // address, and the pointer preview is what inspects the target.
    using UTF8TextPtrNode  = ScalarNode<NodeKind::utf8_text_ptr, 8>;
    using UTF16TextPtrNode = ScalarNode<NodeKind::utf16_text_ptr, 8>;
    using UTF32TextPtrNode = ScalarNode<NodeKind::utf32_text_ptr, 8>;
}
