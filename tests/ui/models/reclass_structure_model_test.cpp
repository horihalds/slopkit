#include <catch2/catch.hpp>

#include <cstddef>
#include <utility>

#include "reclass/node.hpp"
#include "reclass/project.hpp"
#include "support/reclass_helpers.hpp"
#include "support/ui_helpers.hpp"
#include "ui/models/reclass_structure_model.hpp"

namespace
{
    using slopkit::reclass::Class;
    using slopkit::reclass::NodeKind;
    using slopkit::reclass::NodePtr;
    using slopkit::reclass::Project;
    using slopkit::test::node_of;
    using slopkit::ui::models::ReclassStructureModel;
    using Model = ReclassStructureModel;

    QString cell(const Model& model, const QModelIndex& index, int column)
    {
        return model.data(index.sibling(index.row(), column), Qt::DisplayRole).toString();
    }

    std::size_t node_id_of(const Class& type, std::size_t index)
    {
        return type.nodes[index]->id;
    }

    // Player { int8 tag; Point origin; } plus Point { int32 x; float32 y; }
    Project two_classes()
    {
        Project project;
        project.add_class("Point", "app+0x20");
        project.add_class("Player", "app+0x10");

        auto& point = project.classes()[0];
        point.nodes.push_back(node_of(project, NodeKind::int32, "x"));
        point.nodes.push_back(node_of(project, NodeKind::float32, "y"));

        auto& player = project.classes()[1];
        player.nodes.push_back(node_of(project, NodeKind::int8, "tag"));
        NodePtr origin     = node_of(project, NodeKind::class_instance, "origin");
        origin->class_name = "Point";
        player.nodes.push_back(std::move(origin));

        return project;
    }
}

TEST_CASE("the grid lists the selected class's nodes with derived offsets", "[reclass][ui]")
{
    application();

    Project               project;
    ReclassStructureModel model {project};
    project.add_class("Player", "app+0x10");
    Class& player = project.classes().front();
    player.nodes.push_back(node_of(project, NodeKind::int8, "flag"));
    player.nodes.push_back(node_of(project, NodeKind::int32, "health"));
    player.nodes.push_back(node_of(project, NodeKind::float64, "position"));
    model.set_project(project);

    REQUIRE(model.rowCount() == 3);
    CHECK(model.columnCount() == Model::column_count);

    CHECK(model.headerData(Model::address, Qt::Horizontal, Qt::DisplayRole).toString() == QStringLiteral("Address"));
    CHECK(model.headerData(Model::offset, Qt::Horizontal, Qt::DisplayRole).toString() == QStringLiteral("Offset"));
    CHECK(model.headerData(Model::type, Qt::Horizontal, Qt::DisplayRole).toString() == QStringLiteral("Type"));
    CHECK(model.headerData(Model::name, Qt::Horizontal, Qt::DisplayRole).toString() == QStringLiteral("Name"));
    CHECK(model.headerData(Model::value, Qt::Horizontal, Qt::DisplayRole).toString() == QStringLiteral("Value"));
    CHECK(model.headerData(Model::comment, Qt::Horizontal, Qt::DisplayRole).toString() == QStringLiteral("Comment"));

    CHECK(cell(model, model.index(0, 0), Model::name) == QStringLiteral("flag"));
    CHECK(cell(model, model.index(0, 0), Model::type) == QStringLiteral("Int8"));
    CHECK(cell(model, model.index(1, 0), Model::offset) == QStringLiteral("1"));
    CHECK(cell(model, model.index(2, 0), Model::offset) == QStringLiteral("5"));
    CHECK(cell(model, model.index(2, 0), Model::type) == QStringLiteral("Double"));

    // The live columns belong to the value model.
    CHECK_FALSE(model.data(model.index(0, Model::address), Qt::DisplayRole).isValid());
    CHECK_FALSE(model.data(model.index(0, Model::value), Qt::DisplayRole).isValid());

    CHECK(model.blocks().size() == 1);
    CHECK(model.blocks()[0].size == 13);
}

TEST_CASE("an expanded class instance inlines its class beneath the node", "[reclass][ui]")
{
    application();

    Project               project = two_classes();
    ReclassStructureModel model {project};
    model.set_class(1);

    REQUIRE(model.rowCount() == 2);
    CHECK(model.rowCount(model.index(1, 0)) == 0);
    CHECK(cell(model, model.index(1, 0), Model::type) == QStringLiteral("Instance <Point>"));
    CHECK(model.blocks().size() == 1);

    model.set_expanded(node_id_of(project.classes()[1], 1), true);

    REQUIRE(model.rowCount() == 2);
    REQUIRE(model.rowCount(model.index(1, 0)) == 2);

    const QModelIndex inlined = model.index(0, 0, model.index(1, 0));
    CHECK(cell(model, inlined, Model::name) == QStringLiteral("x"));
    CHECK(cell(model, inlined, Model::offset) == QStringLiteral("1"));
    CHECK(cell(model, model.index(1, 0, model.index(1, 0)), Model::offset) == QStringLiteral("5"));

    REQUIRE(model.blocks().size() == 2);
    CHECK(model.blocks()[1].parent_block == 0);
    CHECK(model.blocks()[1].base_offset == 1);
    CHECK(model.blocks()[1].size == 8);
    CHECK_FALSE(model.blocks()[1].via_pointer);

    CHECK(cell(model, model.index(1, 0), Model::offset) == QStringLiteral("1"));
}

TEST_CASE("a class pointer inlines its target through one memory read", "[reclass][ui]")
{
    application();

    Project               project = two_classes();
    ReclassStructureModel model {project};

    Class& player = project.classes()[1];
    player.nodes.erase(player.nodes.begin() + 1);
    NodePtr next     = node_of(project, NodeKind::class_pointer, "next");
    next->class_name = "Point";
    player.nodes.push_back(std::move(next));
    model.set_class(1);

    CHECK(cell(model, model.index(1, 0), Model::type) == QStringLiteral("Pointer <Point>"));
    REQUIRE(model.rowCount(model.index(1, 0)) == 0);

    model.set_expanded(node_id_of(player, 1), true);

    REQUIRE(model.rowCount(model.index(1, 0)) == 2);
    CHECK(cell(model, model.index(0, 0, model.index(1, 0)), Model::name) == QStringLiteral("x"));

    REQUIRE(model.blocks().size() == 2);
    CHECK(model.blocks()[1].via_pointer);
    CHECK(model.blocks()[1].base_offset == 1);
    CHECK(model.blocks()[1].size == 8);
}

TEST_CASE("a class that contains itself stops at a cycle marker", "[reclass][ui]")
{
    application();

    Project               project;
    ReclassStructureModel model {project};
    project.add_class("Self", "app+0x10");
    Class& self = project.classes().front();
    self.nodes.push_back(node_of(project, NodeKind::int32, "x"));
    NodePtr me     = node_of(project, NodeKind::class_instance, "me");
    me->class_name = "Self";
    self.nodes.push_back(std::move(me));
    model.set_project(project);

    model.set_expanded(node_id_of(self, 1), true);

    REQUIRE(model.rowCount() == 2);
    REQUIRE(model.rowCount(model.index(1, 0)) == 2);

    const QModelIndex inner  = model.index(1, 0, model.index(1, 0));
    const QModelIndex marker = model.index(0, 0, model.index(1, 0));
    CHECK(cell(model, marker, Model::name) == QStringLiteral("x"));
    CHECK(cell(model, inner, Model::type) == QStringLiteral("cycle"));
    CHECK(model.rowCount(inner) == 0);

    // The chain stays one class deep: the outer class and one inlined copy.
    CHECK(model.blocks().size() == 2);
}

TEST_CASE("a reference to a missing class paints unknown class", "[reclass][ui]")
{
    application();

    Project               project;
    ReclassStructureModel model {project};
    project.add_class("Holder", "app+0x10");
    Class&  holder    = project.classes().front();
    NodePtr ghost     = node_of(project, NodeKind::class_instance, "ghost");
    ghost->class_name = "Missing";
    holder.nodes.push_back(std::move(ghost));
    model.set_project(project);

    model.set_expanded(node_id_of(holder, 0), true);

    CHECK(cell(model, model.index(0, 0), Model::type) == QStringLiteral("unknown class"));
    CHECK(model.rowCount(model.index(0, 0)) == 0);
    CHECK(model.blocks().size() == 1);
}

TEST_CASE("the grid is empty while no class is selected", "[reclass][ui]")
{
    application();

    Project               project = two_classes();
    ReclassStructureModel model {project};

    CHECK(model.valid_class());

    model.set_class(9);

    CHECK_FALSE(model.valid_class());
    CHECK(model.rowCount() == 0);
    CHECK(model.blocks().empty());
}

TEST_CASE("a row is reachable by its node id", "[reclass][ui]")
{
    application();

    Project               project = two_classes();
    ReclassStructureModel model {project};
    model.set_class(1);

    const QModelIndex index = model.index_for_node(node_id_of(project.classes()[1], 0));
    REQUIRE(index.isValid());
    REQUIRE(model.row_for(index) != nullptr);
    CHECK(model.row_for(index)->node_index == 0);
    CHECK(model.parent(index) == QModelIndex());
    CHECK_FALSE(model.index_for_node(9999).isValid());
}

TEST_CASE("selecting another class drops the inlined references", "[reclass][ui]")
{
    application();

    Project               project = two_classes();
    ReclassStructureModel model {project};
    model.set_class(1);
    model.set_expanded(node_id_of(project.classes()[1], 1), true);
    CHECK(model.expanded_nodes().size() == 1);

    model.set_class(0);

    CHECK(model.expanded_nodes().empty());
    CHECK(model.blocks().size() == 1);
    CHECK(model.blocks()[0].class_index == 0);

    model.set_class(1);
    CHECK(model.rowCount(model.index(1, 0)) == 0);
}

TEST_CASE("a node's name is edited in place through the model", "[reclass][ui]")
{
    application();

    Project               project = two_classes();
    ReclassStructureModel model {project};
    model.set_class(1);

    std::vector<QString> messages;
    QObject::connect(&model,
                     &ReclassStructureModel::statusChanged,
                     [&messages](const QString& message, bool)
                     {
                         messages.push_back(message);
                     });

    QModelIndex announced;
    QObject::connect(&model,
                     &QAbstractItemModel::dataChanged,
                     [&announced](const QModelIndex& top_left, const QModelIndex&, const QList<int>&)
                     {
                         announced = top_left;
                     });

    // The name is the one cell the grid types into, and it offers the editor
    // the same text the cell shows.
    const QModelIndex name_cell = model.index(0, Model::name);
    CHECK(model.flags(name_cell).testFlag(Qt::ItemIsEditable));
    CHECK_FALSE(model.flags(model.index(0, Model::offset)).testFlag(Qt::ItemIsEditable));
    CHECK(model.data(name_cell, Qt::EditRole).toString() == QStringLiteral("tag"));
    CHECK_FALSE(model.data(model.index(0, Model::offset), Qt::EditRole).isValid());

    CHECK(model.setData(name_cell, QStringLiteral("  health  "), Qt::EditRole));
    CHECK(project.classes()[1].nodes[0]->name == "health");
    CHECK(cell(model, model.index(0, 0), Model::name) == QStringLiteral("health"));
    // Only the one cell is announced, so the grid repaints nothing else.
    CHECK(announced.row() == 0);
    CHECK(announced.column() == Model::name);
    REQUIRE(messages.size() == 1);
    CHECK(messages.front() == QStringLiteral("Node renamed."));

    // An empty name is refused and the node keeps the name it had.
    CHECK_FALSE(model.setData(name_cell, QStringLiteral("   "), Qt::EditRole));
    CHECK(project.classes()[1].nodes[0]->name == "health");
    REQUIRE(messages.size() == 2);
    CHECK(messages.back() == QStringLiteral("A node name is required."));

    // No other column and no other role takes an edit.
    CHECK_FALSE(model.setData(model.index(0, Model::offset), QStringLiteral("9"), Qt::EditRole));
    CHECK_FALSE(model.setData(name_cell, QStringLiteral("other"), Qt::DisplayRole));
    CHECK(project.classes()[1].nodes[0]->name == "health");
}

TEST_CASE("a container inlines its children and shows its count", "[reclass][ui]")
{
    application();

    Project project;
    project.add_class("Player", "app+0x10");
    auto& player = project.classes().front();
    player.nodes.push_back(node_of(project, NodeKind::int8, "tag"));
    player.nodes.push_back(slopkit::test::array_of(project, 2, NodeKind::int32));

    Model model {project};
    REQUIRE(model.rowCount() == 2);
    CHECK(cell(model, model.index(1, 0), Model::type) == QStringLiteral("Array [2]"));
    REQUIRE(model.rowCount(model.index(1, 0)) == 0);

    model.set_expanded(player.nodes[1]->id, true);

    REQUIRE(model.rowCount(model.index(1, 0)) == 2);
    CHECK(cell(model, model.index(0, 0, model.index(1, 0)), Model::type) == QStringLiteral("Int32"));
    CHECK(cell(model, model.index(1, 0, model.index(1, 0)), Model::type) == QStringLiteral("Int32"));
}

TEST_CASE("a union's members all sit at the union's own offset", "[reclass][ui]")
{
    application();

    Project project;
    project.add_class("Player", "app+0x10");
    auto& player = project.classes().front();
    player.nodes.push_back(slopkit::test::union_of(project, {NodeKind::int8, NodeKind::int64}));

    Model model {project};
    CHECK(cell(model, model.index(0, 0), Model::type) == QStringLiteral("Union"));

    model.set_expanded(player.nodes[0]->id, true);

    REQUIRE(model.rowCount(model.index(0, 0)) == 2);
    const QModelIndex first  = model.index(0, 0, model.index(0, 0));
    const QModelIndex second = model.index(1, 0, model.index(0, 0));
    CHECK(cell(model, first, Model::offset) == cell(model, second, Model::offset));
}
