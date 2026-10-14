#include <catch2/catch.hpp>

#include <cstddef>
#include <string>

#include "reclass/node.hpp"
#include "reclass/project.hpp"
#include "support/reclass_helpers.hpp"

namespace
{
    using slopkit::reclass::layout_class;
    using slopkit::reclass::node_size;
    using slopkit::reclass::NodeKind;
    using slopkit::reclass::Project;
    using slopkit::test::node_of;
}

TEST_CASE("classes are added by name and looked up", "[reclass]")
{
    Project project;

    const std::size_t first = project.add_class("Player", "app+0x10");
    REQUIRE(first != 0);
    REQUIRE(project.classes().size() == 1);
    REQUIRE(project.class_at(0) != nullptr);
    REQUIRE(project.class_at(0)->name == "Player");
    REQUIRE(project.class_at(0)->address_expression == "app+0x10");

    const std::size_t second = project.add_class("Enemy", "app+0x20");
    REQUIRE(second != first);
    REQUIRE(project.find_class("Enemy") == project.class_at(1));
    REQUIRE(project.find_class("nobody") == nullptr);
    REQUIRE(project.class_at(2) == nullptr);

    REQUIRE(project.add_class("Player", "app+0x30") == 0);
    REQUIRE(project.add_class("", "app+0x30") == 0);
    REQUIRE(project.classes().size() == 2);

    project.remove_class(0);
    REQUIRE(project.classes().size() == 1);
    REQUIRE(project.find_class("Player") == nullptr);
    REQUIRE(project.find_class("Enemy") != nullptr);

    project.clear();
    REQUIRE(project.classes().empty());
}

TEST_CASE("node ids are unique across the project", "[reclass]")
{
    Project project;

    const std::size_t first  = project.next_node_id();
    const std::size_t second = project.next_node_id();
    REQUIRE(second == first + 1);

    project.clear();
    REQUIRE(project.next_node_id() == first);
}

TEST_CASE("offsets accumulate in node order without alignment", "[reclass]")
{
    Project project;
    project.add_class("Player", "app+0x10");
    auto& type = project.classes().front();

    type.nodes.push_back(node_of(project, NodeKind::int8));
    type.nodes.push_back(node_of(project, NodeKind::int32));
    type.nodes.push_back(node_of(project, NodeKind::int16));
    type.nodes.push_back(node_of(project, NodeKind::float64));

    const auto layout = layout_class(project, type);
    REQUIRE(layout.has_value());
    REQUIRE(layout->offsets == std::vector<std::size_t> {0, 1, 5, 7});
    REQUIRE(layout->size == 15);
}

TEST_CASE("the hex kinds accumulate their own widths", "[reclass]")
{
    Project project;
    project.add_class("Player", "app+0x10");
    auto& type = project.classes().front();

    type.nodes.push_back(node_of(project, NodeKind::hex8));
    type.nodes.push_back(node_of(project, NodeKind::hex16));
    type.nodes.push_back(node_of(project, NodeKind::hex32));
    type.nodes.push_back(node_of(project, NodeKind::hex64));

    const auto layout = layout_class(project, type);
    REQUIRE(layout.has_value());
    REQUIRE(layout->offsets == std::vector<std::size_t> {0, 1, 3, 7});
    REQUIRE(layout->size == 15);
}

TEST_CASE("a class instance is sized by the class it names", "[reclass]")
{
    Project project;
    project.add_class("Point", "app+0x10");
    project.add_class("Holder", "app+0x20");

    auto& point = project.classes()[0];
    point.nodes.push_back(node_of(project, NodeKind::int32));
    point.nodes.push_back(node_of(project, NodeKind::int32));

    auto& holder = project.classes()[1];
    holder.nodes.push_back(node_of(project, NodeKind::int8));
    slopkit::reclass::Node instance = node_of(project, NodeKind::class_instance);
    instance.class_name             = "Point";
    holder.nodes.push_back(std::move(instance));
    holder.nodes.push_back(node_of(project, NodeKind::int32));

    const auto layout = layout_class(project, holder);
    REQUIRE(layout.has_value());
    REQUIRE(layout->offsets == std::vector<std::size_t> {0, 1, 9});
    REQUIRE(layout->size == 13);

    REQUIRE(node_size(project, holder.nodes[1]) == 8);
}

TEST_CASE("a class pointer and a plain pointer take a pointer each", "[reclass]")
{
    Project project;
    project.add_class("Point", "app+0x10");
    project.add_class("Holder", "app+0x20");

    project.classes()[0].nodes.push_back(node_of(project, NodeKind::int32));

    slopkit::reclass::Node pointer = node_of(project, NodeKind::class_pointer);
    pointer.class_name             = "Point";

    auto& holder = project.classes()[1];
    holder.nodes.push_back(node_of(project, NodeKind::pointer));
    holder.nodes.push_back(std::move(pointer));

    const auto layout = layout_class(project, holder);
    REQUIRE(layout.has_value());
    REQUIRE(layout->offsets == std::vector<std::size_t> {0, 8});
    REQUIRE(layout->size == 16);
}

TEST_CASE("an unknown class reference takes no space", "[reclass]")
{
    Project project;
    project.add_class("Holder", "app+0x20");
    auto& holder = project.classes().front();

    holder.nodes.push_back(node_of(project, NodeKind::int8));
    slopkit::reclass::Node instance = node_of(project, NodeKind::class_instance);
    instance.class_name             = "Missing";
    holder.nodes.push_back(std::move(instance));
    holder.nodes.push_back(node_of(project, NodeKind::int32));

    const auto layout = layout_class(project, holder);
    REQUIRE(layout.has_value());
    REQUIRE(layout->offsets == std::vector<std::size_t> {0, 1, 1});
    REQUIRE(layout->size == 5);

    REQUIRE(node_size(project, holder.nodes[1]) == 0);
}

TEST_CASE("a class that contains itself is refused", "[reclass]")
{
    Project project;
    project.add_class("Self", "app+0x10");
    auto& self = project.classes().front();

    slopkit::reclass::Node instance = node_of(project, NodeKind::class_instance);
    instance.class_name             = "Self";
    self.nodes.push_back(std::move(instance));

    const auto layout = layout_class(project, self);
    REQUIRE_FALSE(layout.has_value());
    REQUIRE(layout.error().find("Self") != std::string::npos);
    REQUIRE(layout.error().find("cyclic") != std::string::npos);
}

TEST_CASE("a cyclic chain of classes is refused and named", "[reclass]")
{
    Project project;
    project.add_class("A", "app+0x10");
    project.add_class("B", "app+0x20");

    slopkit::reclass::Node to_b = node_of(project, NodeKind::class_instance);
    to_b.class_name             = "B";
    project.classes()[0].nodes.push_back(std::move(to_b));

    slopkit::reclass::Node to_a = node_of(project, NodeKind::class_instance);
    to_a.class_name             = "A";
    project.classes()[1].nodes.push_back(std::move(to_a));

    const auto layout = layout_class(project, project.classes()[0]);
    REQUIRE_FALSE(layout.has_value());
    REQUIRE(layout.error().find("A") != std::string::npos);
    REQUIRE(layout.error().find("B") != std::string::npos);
    REQUIRE(layout.error().find("cyclic") != std::string::npos);
}

TEST_CASE("a cyclic reference is sized as zero outside the layout path", "[reclass]")
{
    Project project;
    project.add_class("A", "app+0x10");
    project.add_class("B", "app+0x20");

    slopkit::reclass::Node to_b = node_of(project, NodeKind::class_instance);
    to_b.class_name             = "B";
    project.classes()[0].nodes.push_back(std::move(to_b));

    slopkit::reclass::Node to_a = node_of(project, NodeKind::class_instance);
    to_a.class_name             = "A";
    project.classes()[1].nodes.push_back(std::move(to_a));

    REQUIRE(node_size(project, project.classes()[0].nodes[0]) == 0);
}

TEST_CASE("an empty class lays out as zero bytes", "[reclass]")
{
    Project project;
    project.add_class("Empty", "app+0x10");

    const auto layout = layout_class(project, project.classes().front());
    REQUIRE(layout.has_value());
    REQUIRE(layout->offsets.empty());
    REQUIRE(layout->size == 0);
}
