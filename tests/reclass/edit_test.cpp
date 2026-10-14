#include <catch2/catch.hpp>

#include <cstddef>
#include <string>
#include <vector>

#include "reclass/edit.hpp"
#include "reclass/node.hpp"
#include "reclass/project.hpp"
#include "support/reclass_helpers.hpp"

namespace
{
    using slopkit::reclass::append_bytes;
    using slopkit::reclass::change_node_kind;
    using slopkit::reclass::layout_class;
    using slopkit::reclass::NodeKind;
    using slopkit::reclass::pad_nodes;
    using slopkit::reclass::Project;
    using slopkit::test::node_of;

    std::vector<NodeKind> kinds_of(const std::vector<slopkit::reclass::Node>& nodes)
    {
        std::vector<NodeKind> kinds;
        for (const slopkit::reclass::Node& node : nodes)
        {
            kinds.push_back(node.kind);
        }
        return kinds;
    }

    std::vector<std::size_t> offsets_of(const Project& project, std::size_t index)
    {
        const auto layout = layout_class(project, project.classes()[index]);
        REQUIRE(layout.has_value());
        return layout->offsets;
    }
}

TEST_CASE("padding covers a byte count with the widest hex nodes", "[reclass]")
{
    REQUIRE(kinds_of(pad_nodes(0)).empty());
    REQUIRE(kinds_of(pad_nodes(1)) == std::vector<NodeKind> {NodeKind::hex8});
    REQUIRE(kinds_of(pad_nodes(2)) == std::vector<NodeKind> {NodeKind::hex16});
    REQUIRE(kinds_of(pad_nodes(3)) == std::vector<NodeKind> {NodeKind::hex16, NodeKind::hex8});
    REQUIRE(kinds_of(pad_nodes(4)) == std::vector<NodeKind> {NodeKind::hex32});
    REQUIRE(kinds_of(pad_nodes(6)) == std::vector<NodeKind> {NodeKind::hex32, NodeKind::hex16});
    REQUIRE(kinds_of(pad_nodes(7)) == std::vector<NodeKind> {NodeKind::hex32, NodeKind::hex16, NodeKind::hex8});
    REQUIRE(kinds_of(pad_nodes(8)) == std::vector<NodeKind> {NodeKind::hex64});
    REQUIRE(kinds_of(pad_nodes(12)) == std::vector<NodeKind> {NodeKind::hex64, NodeKind::hex32});
}

TEST_CASE("appended bytes become uniquely identified pad nodes", "[reclass]")
{
    Project project;
    project.add_class("Player", "app+0x10");
    auto& type = project.classes().front();

    type.nodes.push_back(node_of(project, NodeKind::int32));
    append_bytes(project, type.nodes, 6);

    REQUIRE(kinds_of(type.nodes) == std::vector<NodeKind> {NodeKind::int32, NodeKind::hex32, NodeKind::hex16});
    REQUIRE(type.nodes[1].id != 0);
    REQUIRE(type.nodes[2].id != 0);
    REQUIRE(type.nodes[1].id != type.nodes[2].id);
    REQUIRE(offsets_of(project, 0) == std::vector<std::size_t> {0, 4, 8});
}

TEST_CASE("shrinking a node's kind pads the freed bytes", "[reclass]")
{
    Project project;
    project.add_class("Player", "app+0x10");
    auto& type = project.classes().front();

    type.nodes.push_back(node_of(project, NodeKind::int64, "big"));
    type.nodes.push_back(node_of(project, NodeKind::int32, "small"));
    REQUIRE(offsets_of(project, 0) == std::vector<std::size_t> {0, 8});

    change_node_kind(project, type.nodes, 0, NodeKind::int32);

    REQUIRE(kinds_of(type.nodes) == std::vector<NodeKind> {NodeKind::int32, NodeKind::hex32, NodeKind::int32});
    REQUIRE(type.nodes[0].name == "big");
    REQUIRE(type.nodes[2].name == "small");
    REQUIRE(offsets_of(project, 0) == std::vector<std::size_t> {0, 4, 8});
}

TEST_CASE("growing a node's kind inserts nothing", "[reclass]")
{
    Project project;
    project.add_class("Player", "app+0x10");
    auto& type = project.classes().front();

    type.nodes.push_back(node_of(project, NodeKind::int16));
    type.nodes.push_back(node_of(project, NodeKind::int32));

    change_node_kind(project, type.nodes, 0, NodeKind::int64);

    REQUIRE(kinds_of(type.nodes) == std::vector<NodeKind> {NodeKind::int64, NodeKind::int32});
    REQUIRE(offsets_of(project, 0) == std::vector<std::size_t> {0, 8});
}

TEST_CASE("re-typing a class instance pads by the class it held", "[reclass]")
{
    Project project;
    project.add_class("Point", "app+0x10");
    project.add_class("Holder", "app+0x20");

    project.classes()[0].nodes.push_back(node_of(project, NodeKind::int32));

    auto&                  holder   = project.classes()[1];
    slopkit::reclass::Node instance = node_of(project, NodeKind::class_instance, "p");
    instance.class_name             = "Point";
    holder.nodes.push_back(std::move(instance));
    holder.nodes.push_back(node_of(project, NodeKind::int32, "x"));
    REQUIRE(offsets_of(project, 1) == std::vector<std::size_t> {0, 4});

    change_node_kind(project, holder.nodes, 0, NodeKind::int8);

    REQUIRE(kinds_of(holder.nodes)
            == std::vector<NodeKind> {NodeKind::int8, NodeKind::hex16, NodeKind::hex8, NodeKind::int32});
    REQUIRE(holder.nodes[0].class_name.empty());
    REQUIRE(offsets_of(project, 1) == std::vector<std::size_t> {0, 1, 3, 4});
}

TEST_CASE("a node that stops referring to a class drops the name", "[reclass]")
{
    Project project;
    project.add_class("Point", "app+0x10");
    project.add_class("Holder", "app+0x20");

    project.classes()[0].nodes.push_back(node_of(project, NodeKind::int32));

    auto&                  holder  = project.classes()[1];
    slopkit::reclass::Node pointer = node_of(project, NodeKind::class_pointer, "next");
    pointer.class_name             = "Point";
    holder.nodes.push_back(std::move(pointer));

    change_node_kind(project, holder.nodes, 0, NodeKind::uint64);

    REQUIRE(holder.nodes[0].kind == NodeKind::uint64);
    REQUIRE(holder.nodes[0].class_name.empty());
    REQUIRE(holder.nodes[0].name == "next");
}

TEST_CASE("re-typing out of range is ignored", "[reclass]")
{
    Project project;
    project.add_class("Player", "app+0x10");
    auto& type = project.classes().front();

    type.nodes.push_back(node_of(project, NodeKind::int32));
    change_node_kind(project, type.nodes, 4, NodeKind::int8);

    REQUIRE(kinds_of(type.nodes) == std::vector<NodeKind> {NodeKind::int32});
}
