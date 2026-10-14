#include <catch2/catch.hpp>

#include <array>
#include <cstddef>
#include <string_view>

#include "reclass/node.hpp"

namespace
{
    using slopkit::reclass::describe;
    using slopkit::reclass::is_class_kind;
    using slopkit::reclass::is_editable;
    using slopkit::reclass::is_hex_kind;
    using slopkit::reclass::kind_from_token;
    using slopkit::reclass::kind_size;
    using slopkit::reclass::NodeKind;
    using slopkit::reclass::type_token;

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
