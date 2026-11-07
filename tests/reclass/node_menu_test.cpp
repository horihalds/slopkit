#include <catch2/catch.hpp>

#include <cstddef>
#include <vector>

#include "reclass/node_menu.hpp"
#include "reclass/nodes/node.hpp"
#include "reclass/nodes/number.hpp"
#include "reclass/project.hpp"
#include "reclass/value.hpp"
#include "support/reclass_helpers.hpp"

namespace
{
    using slopkit::reclass::format_node_value_text;
    using slopkit::reclass::kind_size;
    using slopkit::reclass::make_node;
    using slopkit::reclass::node_menu_groups;
    using slopkit::reclass::node_menu_label;
    using slopkit::reclass::NodeKind;
    using slopkit::reclass::NodeMenuEntry;
    using slopkit::reclass::NodeMenuGroup;
    using slopkit::reclass::NodePtr;
    using slopkit::reclass::Project;
}

TEST_CASE("the node menu lists the reference's groups in its order", "[reclass]")
{
    const auto groups = node_menu_groups();
    REQUIRE(groups.size() == 10);

    CHECK(groups[0].label == "Hex");
    CHECK(groups[0].entries.front().kind == NodeKind::hex64);
    CHECK(groups[0].entries.back().kind == NodeKind::hex8);

    CHECK(groups[1].entries.front().kind == NodeKind::nint);
    CHECK(groups[2].entries.front().kind == NodeKind::nuint);

    CHECK(groups[3].entries.front().kind == NodeKind::boolean);
    CHECK(groups[3].entries.back().kind == NodeKind::enum_node);

    CHECK(groups[4].entries.front().kind == NodeKind::float32);

    CHECK(groups[5].entries.front().kind == NodeKind::vector4);
    CHECK(groups[5].entries.back().kind == NodeKind::matrix3x3);

    CHECK(groups[6].entries.front().kind == NodeKind::utf8_text);
    CHECK(groups[7].entries.front().kind == NodeKind::pointer);
    CHECK(groups[8].entries.front().kind == NodeKind::class_instance);
    CHECK(groups[9].entries.back().kind == NodeKind::function_ptr);

    // A kind the table lists takes the table's label; one it does not falls back
    // to the taxonomy name.
    CHECK(node_menu_label(NodeKind::function_ptr) == "Function Ptr");
    CHECK(node_menu_label(NodeKind::utf16_text) == "Utf16Text");
    CHECK(node_menu_label(NodeKind::class_pointer) == "Class Pointer");
}

TEST_CASE("the factory builds every menu kind with the size its kind reports", "[reclass]")
{
    Project project;

    for (const NodeMenuGroup& group : node_menu_groups())
    {
        CHECK_FALSE(group.label.empty());
        for (const NodeMenuEntry& entry : group.entries)
        {
            NodePtr node = make_node(entry.kind);
            REQUIRE(node != nullptr);
            CHECK(node->kind() == entry.kind);
            CHECK(node_menu_label(entry.kind) == entry.label);

            // A fixed kind is exactly as large as the taxonomy says; a kind whose
            // size comes from its own parameters answers through that parameter,
            // so only the fixed kinds are compared here.
            if (const std::size_t fixed = kind_size(entry.kind); fixed != 0)
            {
                CHECK(node->size(project) == fixed);
            }
        }
    }
}

TEST_CASE("a text node decodes the target and an enum node names its value", "[reclass]")
{
    Project project;
    project.add_enum("Colour", 4);
    project.enums()[0].values.push_back({.name = "red", .value = 1, .comment = {}});

    const NodePtr text = slopkit::test::node_of(project, NodeKind::utf8_text);
    CHECK(format_node_value_text(*text, project, slopkit::test::bytes_of({'h', 'i'})) == "hi");

    const NodePtr wide = slopkit::test::node_of(project, NodeKind::utf16_text);
    CHECK(format_node_value_text(*wide, project, slopkit::test::bytes_of({0x68, 0x00, 0x69, 0x00})) == "hi");

    const NodePtr bits  = slopkit::test::node_of(project, NodeKind::bitfield);
    auto*         field = dynamic_cast<slopkit::reclass::BitFieldNode*>(bits.get());
    REQUIRE(field != nullptr);
    field->bits = 4;
    CHECK(format_node_value_text(*bits, project, slopkit::test::bytes_of({0x0F})) == "15");

    const NodePtr colour = slopkit::test::node_of(project, NodeKind::enum_node);
    auto*         value  = dynamic_cast<slopkit::reclass::EnumNode*>(colour.get());
    REQUIRE(value != nullptr);
    value->enum_name  = "Colour";
    value->size_bytes = 4;
    CHECK(format_node_value_text(*colour, project, slopkit::test::bytes_of({1, 0, 0, 0})) == "1 (red)");

    // An enum whose enum is gone still reads as a number rather than failing.
    value->enum_name = "Missing";
    CHECK(format_node_value_text(*colour, project, slopkit::test::bytes_of({1, 0, 0, 0})) == "1");
}
