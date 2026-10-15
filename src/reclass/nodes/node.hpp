#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "reclass/node.hpp"

namespace slopkit::reclass
{
    class Project;
    class Node;

    // A node is owned by the class or container that holds it, so a class
    // project is one tree of nodes.
    using NodePtr = std::unique_ptr<Node>;

    // The polymorphic node model: one class per kind, mirroring the reference's
    // BaseNode tree. Every kind answers its own kind tag, its storage size and
    // how it is copied; a container additionally hands out its children.
    class Node
    {
    public:
        Node()          = default;
        virtual ~Node() = default;

        std::size_t id {};
        std::string name;
        std::string comment;
        bool        hidden {false};
        bool        hex {false};
        std::string class_name;

        [[nodiscard]] virtual NodeKind kind() const noexcept = 0;

        // Total storage the node occupies, including its children and the class
        // a class node names.
        [[nodiscard]] virtual std::size_t size(const Project& project) const = 0;

        // The bytes the Value column reads from the node's address; zero when
        // the value is derived from children or the node has none.
        [[nodiscard]] virtual std::size_t value_size() const noexcept = 0;

        // How many times the node's payload repeats; only the kinds that name a
        // class array repeat, so every other kind answers one.
        [[nodiscard]] virtual std::size_t repeat() const noexcept
        {
            return 1;
        }

        [[nodiscard]] virtual NodePtr clone() const = 0;

        [[nodiscard]] virtual std::span<const NodePtr> children() const noexcept
        {
            return {};
        }

        [[nodiscard]] virtual bool is_container() const noexcept
        {
            return false;
        }

        // The per-kind text the Type column shows beside the kind's name.
        [[nodiscard]] virtual std::string_view extra_text() const noexcept
        {
            return {};
        }

        // The number that text brackets and the user may type there: a text
        // node's length in characters, an array's element count. Empty for a
        // kind whose per-kind text is not that number - a bit field's width, an
        // enum's name - so the cell offers no run editor.
        [[nodiscard]] virtual std::optional<std::size_t> extent() const noexcept
        {
            return std::nullopt;
        }

        // Sets that number; a kind without one refuses, and a zero is refused
        // everywhere, since it would leave a text node with no bytes or an
        // array with no element to shape the next one from.
        [[nodiscard]] virtual bool set_extent(Project& project, std::size_t value)
        {
            static_cast<void>(project);
            static_cast<void>(value);
            return false;
        }
    };

    // A fixed-size node that carries no per-kind state: the numeric, hex and
    // plain pointer kinds.
    template<NodeKind Kind, std::size_t Bytes>
    class ScalarNode : public Node
    {
    public:
        [[nodiscard]] NodeKind kind() const noexcept override
        {
            return Kind;
        }

        [[nodiscard]] std::size_t size(const Project&) const override
        {
            return Bytes;
        }

        [[nodiscard]] std::size_t value_size() const noexcept override
        {
            return Bytes;
        }

        [[nodiscard]] NodePtr clone() const override
        {
            return std::make_unique<ScalarNode>(*this);
        }
    };

    // Builds a node of `kind` with no id, name or other per-node state.
    [[nodiscard]] NodePtr make_node(NodeKind kind);

    // The `[N]` a kind brackets beside its name: a text node's length, an
    // array's element count.
    [[nodiscard]] std::string count_label(std::size_t count);

}
