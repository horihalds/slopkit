#include <catch2/catch.hpp>

#include <array>
#include <cstddef>
#include <string_view>

#include "reclass/node.hpp"
#include "reclass/nodes/node.hpp"
#include "reclass/project.hpp"
#include "support/reclass_helpers.hpp"

namespace
{
    using slopkit::reclass::describe;
    using slopkit::reclass::is_class_kind;
    using slopkit::reclass::is_editable;
    using slopkit::reclass::is_hex_kind;
    using slopkit::reclass::kind_from_token;
    using slopkit::reclass::kind_size;
    using slopkit::reclass::NodeKind;
    using slopkit::reclass::NodePtr;
    using slopkit::reclass::Project;
    using slopkit::reclass::type_token;
    using slopkit::test::array_of;
    using slopkit::test::instance_array_of;
    using slopkit::test::node_of;
    using slopkit::test::wrapper_array_of;

    constexpr std::array kAllKinds {
        NodeKind::int8,    NodeKind::int16,     NodeKind::int32,          NodeKind::int64,         NodeKind::uint8,
        NodeKind::uint16,  NodeKind::uint32,    NodeKind::uint64,         NodeKind::float32,       NodeKind::float64,
        NodeKind::boolean, NodeKind::character, NodeKind::hex8,           NodeKind::hex16,         NodeKind::hex32,
        NodeKind::hex64,   NodeKind::pointer,   NodeKind::class_instance, NodeKind::class_pointer,
    };
}

TEST_CASE("every node kind reports the bytes it occupies", "[reclass]")
{
    REQUIRE(kind_size(NodeKind::int8) == 1);
    REQUIRE(kind_size(NodeKind::int16) == 2);
    REQUIRE(kind_size(NodeKind::int32) == 4);
    REQUIRE(kind_size(NodeKind::int64) == 8);
    REQUIRE(kind_size(NodeKind::uint8) == 1);
    REQUIRE(kind_size(NodeKind::uint16) == 2);
    REQUIRE(kind_size(NodeKind::uint32) == 4);
    REQUIRE(kind_size(NodeKind::uint64) == 8);
    REQUIRE(kind_size(NodeKind::float32) == 4);
    REQUIRE(kind_size(NodeKind::float64) == 8);
    REQUIRE(kind_size(NodeKind::boolean) == 1);
    REQUIRE(kind_size(NodeKind::character) == 1);
    REQUIRE(kind_size(NodeKind::hex8) == 1);
    REQUIRE(kind_size(NodeKind::hex16) == 2);
    REQUIRE(kind_size(NodeKind::hex32) == 4);
    REQUIRE(kind_size(NodeKind::hex64) == 8);
    REQUIRE(kind_size(NodeKind::pointer) == 8);
    REQUIRE(kind_size(NodeKind::class_pointer) == 8);
    REQUIRE(kind_size(NodeKind::class_instance) == 0);
}

TEST_CASE("every kind carries a stable token and a description", "[reclass]")
{
    for (const NodeKind kind : kAllKinds)
    {
        const std::string_view token = type_token(kind);
        REQUIRE_FALSE(token.empty());
        REQUIRE(kind_from_token(token) == kind);
        REQUIRE_FALSE(describe(kind).empty());
    }

    REQUIRE_FALSE(kind_from_token("int32").has_value());
    REQUIRE_FALSE(kind_from_token("").has_value());
}

TEST_CASE("the type tokens are the persisted spelling", "[reclass]")
{
    REQUIRE(type_token(NodeKind::int32) == "i32");
    REQUIRE(type_token(NodeKind::uint64) == "u64");
    REQUIRE(type_token(NodeKind::float32) == "f32");
    REQUIRE(type_token(NodeKind::boolean) == "bool");
    REQUIRE(type_token(NodeKind::character) == "char");
    REQUIRE(type_token(NodeKind::hex64) == "hex64");
    REQUIRE(type_token(NodeKind::pointer) == "pointer");
    REQUIRE(type_token(NodeKind::class_instance) == "class_instance");
    REQUIRE(type_token(NodeKind::class_pointer) == "class_pointer");
}

TEST_CASE("only scalar values can be typed back into the target", "[reclass]")
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
                                NodeKind::character})
    {
        REQUIRE(is_editable(kind));
    }

    REQUIRE_FALSE(is_editable(NodeKind::hex8));
    REQUIRE_FALSE(is_editable(NodeKind::hex32));
    REQUIRE_FALSE(is_editable(NodeKind::pointer));
    REQUIRE_FALSE(is_editable(NodeKind::class_instance));
    REQUIRE_FALSE(is_editable(NodeKind::class_pointer));
}

TEST_CASE("class and hex kinds are recognised", "[reclass]")
{
    REQUIRE(is_class_kind(NodeKind::class_instance));
    REQUIRE(is_class_kind(NodeKind::class_pointer));
    REQUIRE_FALSE(is_class_kind(NodeKind::pointer));

    REQUIRE(is_hex_kind(NodeKind::hex8));
    REQUIRE(is_hex_kind(NodeKind::hex64));
    REQUIRE_FALSE(is_hex_kind(NodeKind::int32));
}

TEST_CASE("a text node brackets its length in characters", "[reclass]")
{
    Project project;

    const NodePtr utf8 = node_of(project, NodeKind::utf8_text);
    REQUIRE(utf8->extra_text() == "[16]");
    REQUIRE(utf8->extent() == 16);
    REQUIRE(utf8->set_extent(project, 32));
    REQUIRE(utf8->extent() == 32);
    REQUIRE(utf8->extra_text() == "[32]");
    REQUIRE(utf8->value_size() == 32);

    const NodePtr utf16 = node_of(project, NodeKind::utf16_text);
    REQUIRE(utf16->set_extent(project, 32));
    REQUIRE(utf16->extra_text() == "[32]");
    REQUIRE(utf16->value_size() == 64);

    const NodePtr utf32 = node_of(project, NodeKind::utf32_text);
    REQUIRE(utf32->set_extent(project, 32));
    REQUIRE(utf32->value_size() == 128);

    REQUIRE_FALSE(utf8->set_extent(project, 0));
    REQUIRE(utf8->extent() == 32);
}

TEST_CASE("an array's element count is the number it brackets", "[reclass]")
{
    Project project;

    const NodePtr array = array_of(project, 3, NodeKind::int32);
    REQUIRE(array->extra_text() == "[3]");
    REQUIRE(array->extent() == 3);

    const std::size_t first  = array->children()[0]->id;
    const std::size_t second = array->children()[1]->id;
    const std::size_t third  = array->children()[2]->id;

    REQUIRE(array->set_extent(project, 6));
    REQUIRE(array->extra_text() == "[6]");

    REQUIRE(array->children()[0]->id == first);
    REQUIRE(array->children()[1]->id == second);
    REQUIRE(array->children()[2]->id == third);
    REQUIRE(array->children()[3]->kind() == NodeKind::int32);
    REQUIRE(array->children()[5]->kind() == NodeKind::int32);
    REQUIRE(array->children()[5]->id != 0);
    REQUIRE(array->children()[3]->id != array->children()[4]->id);
    REQUIRE(array->children()[3]->id != third);

    REQUIRE(array->set_extent(project, 1));
    REQUIRE(array->children().size() == 1);
    REQUIRE(array->children()[0]->id == first);

    REQUIRE_FALSE(array->set_extent(project, 0));
    REQUIRE(array->extent() == 1);
}

TEST_CASE("an array with no element grows into the default element", "[reclass]")
{
    Project project;

    // A hand-edited project can hold an array with no element at all.
    const NodePtr array = array_of(project, 0);
    REQUIRE(array->extent() == 0);
    REQUIRE(array->set_extent(project, 2));
    REQUIRE(array->children().size() == 2);
    REQUIRE(array->children()[0]->kind() == NodeKind::int8);
    REQUIRE(array->children()[0]->id != 0);
    REQUIRE(array->children()[0]->id != array->children()[1]->id);
}

TEST_CASE("the class arrays carry their element count", "[reclass]")
{
    Project project;

    const NodePtr instances = instance_array_of(project, "Player", 2);
    REQUIRE(instances->extra_text() == "[2]");
    REQUIRE(instances->extent() == 2);
    REQUIRE(instances->set_extent(project, 4));
    REQUIRE(instances->repeat() == 4);
    REQUIRE_FALSE(instances->set_extent(project, 0));
    REQUIRE(instances->extent() == 4);

    const NodePtr wrappers = wrapper_array_of(project, "Player", 1);
    REQUIRE(wrappers->extra_text() == "[1]");
    REQUIRE(wrappers->set_extent(project, 3));
    REQUIRE(wrappers->repeat() == 3);
    REQUIRE_FALSE(wrappers->set_extent(project, 0));
    REQUIRE(wrappers->extent() == 3);
}

TEST_CASE("a kind whose per-kind text is not a number offers no extent", "[reclass]")
{
    Project project;

    for (const NodeKind kind : {NodeKind::int32,
                                NodeKind::hex32,
                                NodeKind::pointer,
                                NodeKind::class_instance,
                                NodeKind::bitfield,
                                NodeKind::enum_node,
                                NodeKind::union_value,
                                NodeKind::utf8_text_ptr})
    {
        const NodePtr node = node_of(project, kind);
        REQUIRE_FALSE(node->extent().has_value());
        REQUIRE_FALSE(node->set_extent(project, 4));
    }
}
