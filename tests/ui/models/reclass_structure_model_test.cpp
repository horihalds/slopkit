#include <catch2/catch.hpp>

#include <cstddef>
#include <utility>

#include "reclass/edit.hpp"
#include "reclass/node.hpp"
#include "reclass/project.hpp"
#include "support/reclass_helpers.hpp"
#include "support/ui_helpers.hpp"
#include "ui/components/elided_tooltip_delegate.hpp"
#include "ui/models/reclass_structure_model.hpp"
#include "ui/theme.hpp"

namespace
{
    using slopkit::reclass::Class;
    using slopkit::reclass::NodeKind;
    using slopkit::reclass::NodePtr;
    using slopkit::reclass::Project;
    using slopkit::test::array_of;
    using slopkit::test::instance_array_of;
    using slopkit::test::node_of;
    using slopkit::test::wrapper_array_of;
    using slopkit::ui::models::ElementRuns;
    using slopkit::ui::models::kExtentTextRole;
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
    CHECK(model.headerData(Model::ascii, Qt::Horizontal, Qt::DisplayRole).toString() == QStringLiteral("Ascii"));
    CHECK(model.headerData(Model::hints, Qt::Horizontal, Qt::DisplayRole).toString() == QStringLiteral("Hints"));
    CHECK(model.headerData(Model::pointer, Qt::Horizontal, Qt::DisplayRole).toString() == QStringLiteral("Pointer"));

    // The order is the grid's: offset, address, ascii, type, name, value,
    // comment, hints, pointer.
    CHECK(Model::column_count == 9);
    CHECK(Model::offset < Model::address);
    CHECK(Model::address < Model::ascii);
    CHECK(Model::ascii < Model::type);
    CHECK(Model::type < Model::name);
    CHECK(Model::name < Model::value);
    CHECK(Model::value < Model::comment);
    CHECK(Model::comment < Model::hints);
    CHECK(Model::hints < Model::pointer);

    CHECK(cell(model, model.index(0, 0), Model::name) == QStringLiteral("flag"));
    CHECK(cell(model, model.index(0, 0), Model::type) == QStringLiteral("Int8"));
    CHECK(cell(model, model.index(1, 0), Model::offset) == QStringLiteral("1"));
    CHECK(cell(model, model.index(2, 0), Model::offset) == QStringLiteral("5"));
    CHECK(cell(model, model.index(2, 0), Model::type) == QStringLiteral("Double"));

    // The live columns belong to the value model.
    CHECK_FALSE(model.data(model.index(0, Model::address), Qt::DisplayRole).isValid());
    CHECK_FALSE(model.data(model.index(0, Model::value), Qt::DisplayRole).isValid());
    CHECK_FALSE(model.data(model.index(0, Model::hints), Qt::DisplayRole).isValid());
    CHECK_FALSE(model.data(model.index(0, Model::pointer), Qt::DisplayRole).isValid());

    CHECK(model.blocks().size() == 1);
    CHECK(model.blocks()[0].size == 13);
}

TEST_CASE("a hex node draws no kind and only a name it has", "[reclass][ui]")
{
    application();

    Project               project;
    ReclassStructureModel model {project};
    project.add_class("Player", "app+0x10");
    Class& player = project.classes().front();
    player.nodes.push_back(node_of(project, NodeKind::hex64, ""));
    player.nodes.push_back(node_of(project, NodeKind::int32, "health"));
    player.nodes.push_back(node_of(project, NodeKind::hex16, "raw"));
    model.set_project(project);

    // The byte count already shows a hex node's width, so its kind cell stays
    // empty while a typed node spells its own out.
    CHECK(cell(model, model.index(0, 0), Model::type).isEmpty());
    CHECK(cell(model, model.index(1, 0), Model::type) == QStringLiteral("Int32"));
    CHECK(cell(model, model.index(2, 0), Model::type).isEmpty());

    // A hex node whose name the user typed still draws it; the unnamed one
    // stays unnamed.
    CHECK(cell(model, model.index(0, 0), Model::name).isEmpty());
    CHECK(cell(model, model.index(1, 0), Model::name) == QStringLiteral("health"));
    CHECK(cell(model, model.index(2, 0), Model::name) == QStringLiteral("raw"));
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

TEST_CASE("a node's note is edited in place through the model", "[reclass][ui]")
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

    // The note is typed into like the name beside it, and the editor starts
    // from the stored text rather than from the drawn marker.
    const QModelIndex note = model.index(0, Model::comment);
    CHECK(model.flags(note).testFlag(Qt::ItemIsEditable));
    CHECK(model.data(note, Qt::EditRole).toString().isEmpty());
    CHECK_FALSE(model.data(model.index(0, Model::offset), Qt::EditRole).isValid());

    CHECK(model.setData(note, QStringLiteral("  the player's tag  "), Qt::EditRole));
    CHECK(project.classes()[1].nodes[0]->comment == "the player's tag");
    CHECK(model.data(note, Qt::EditRole).toString() == QStringLiteral("the player's tag"));
    CHECK(cell(model, model.index(0, 0), Model::comment) == QStringLiteral("// the player's tag"));
    // Only the one cell is announced, so the grid repaints nothing else.
    CHECK(announced.row() == 0);
    CHECK(announced.column() == Model::comment);
    REQUIRE(messages.size() == 1);
    CHECK(messages.front() == QStringLiteral("Comment saved."));

    // A note is free text, so a note that is only spaces clears it.
    CHECK(model.setData(note, QStringLiteral("   "), Qt::EditRole));
    CHECK(project.classes()[1].nodes[0]->comment.empty());
    CHECK(cell(model, model.index(0, 0), Model::comment) == QStringLiteral("//"));
    REQUIRE(messages.size() == 2);
    CHECK(messages.back() == QStringLiteral("Comment cleared."));

    // No other column and no other role takes an edit.
    CHECK_FALSE(model.setData(note, QStringLiteral("other"), Qt::DisplayRole));
    CHECK_FALSE(model.setData(model.index(0, Model::offset), QStringLiteral("9"), Qt::EditRole));
    CHECK(project.classes()[1].nodes[0]->comment.empty());

    // A hidden node's `...` placeholder takes no note of its own.
    project.classes()[1].nodes[0]->hidden = true;
    model.rebuild();

    const auto* hidden = model.row_at({}, 0);
    REQUIRE(hidden != nullptr);
    REQUIRE(hidden->hidden);
    CHECK_FALSE(model.flags(model.index(0, Model::comment)).testFlag(Qt::ItemIsEditable));
    CHECK_FALSE(model.set_node_comment(*hidden, QStringLiteral("hidden")));
    CHECK(project.classes()[1].nodes[0]->comment.empty());
}

TEST_CASE("an array shows one element at a time and counts its elements", "[reclass][ui]")
{
    application();

    Project project;
    project.add_class("Player", "app+0x10");
    auto& player = project.classes().front();
    player.nodes.push_back(node_of(project, NodeKind::int8, "tag"));
    player.nodes.push_back(slopkit::test::array_of(project, 2, NodeKind::int32));

    Model model {project};
    REQUIRE(model.rowCount() == 2);
    CHECK(cell(model, model.index(1, 0), Model::type) == QStringLiteral("Array<Int32> [2] ◀ (0) ▶"));
    REQUIRE(model.rowCount(model.index(1, 0)) == 0);

    model.set_expanded(player.nodes[1]->id, true);

    // One element row - the one the index names - rather than one per element.
    REQUIRE(model.rowCount(model.index(1, 0)) == 1);
    const QModelIndex element = model.index(0, 0, model.index(1, 0));
    CHECK(cell(model, element, Model::type) == QStringLiteral("Int32"));
    CHECK(cell(model, element, Model::offset) == QStringLiteral("1"));

    // Stepping to the next element moves the one row along with it.
    REQUIRE(model.step_element(player.nodes[1]->id, 1));
    REQUIRE(model.rowCount(model.index(1, 0)) == 1);
    CHECK(cell(model, model.index(0, 0, model.index(1, 0)), Model::offset) == QStringLiteral("5"));
    CHECK(cell(model, model.index(1, 0), Model::type) == QStringLiteral("Array<Int32> [2] ◀ (1) ▶"));
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

TEST_CASE("a note is drawn prefixed and each column takes the theme's role", "[reclass][ui]")
{
    application();

    Project               project;
    ReclassStructureModel model {project};
    project.add_class("Player", "app+0x10");
    Class&  player  = project.classes().front();
    NodePtr tagged  = node_of(project, NodeKind::int32, "health");
    tagged->comment = "the player's health";
    player.nodes.push_back(std::move(tagged));
    player.nodes.push_back(node_of(project, NodeKind::float32, "speed"));
    model.set_project(project);

    const slopkit::ui::Theme& theme = slopkit::ui::active_theme();

    // The cell is prefixed with the marker, the tooltip carries the note alone.
    const QModelIndex note = model.index(0, Model::comment);
    CHECK(model.data(note, Qt::DisplayRole).toString() == QStringLiteral("// the player's health"));
    CHECK(model.data(note, slopkit::ui::widgets::kFullTextRole).toString() == QStringLiteral("the player's health"));
    CHECK(model.data(note, Qt::ForegroundRole).value<QColor>() == theme.grid_comment);

    // A row with no note draws the bare marker, and its tooltip stays empty.
    const QModelIndex empty = model.index(1, Model::comment);
    CHECK(model.data(empty, Qt::DisplayRole).toString() == QStringLiteral("//"));
    CHECK(model.data(empty, slopkit::ui::widgets::kFullTextRole).toString().isEmpty());

    // The offset and the kind's name take their own roles; the name column is
    // left to the view.
    CHECK(model.data(model.index(0, Model::offset), Qt::ForegroundRole).value<QColor>() == theme.grid_offset);
    CHECK(model.data(model.index(0, Model::type), Qt::ForegroundRole).value<QColor>() == theme.datatype_signed);
    CHECK(model.data(model.index(1, Model::type), Qt::ForegroundRole).value<QColor>() == theme.datatype_float);
    CHECK_FALSE(model.data(model.index(0, Model::name), Qt::ForegroundRole).isValid());
}

TEST_CASE("a marked reference row is muted in every column", "[reclass][ui]")
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

    const QColor muted = slopkit::ui::active_theme().text_muted;
    for (const int column : {Model::offset, Model::type, Model::comment})
    {
        CHECK(model.data(model.index(0, column), Qt::ForegroundRole).value<QColor>() == muted);
    }
}

TEST_CASE("the Type cell's bracketed run is answered for the kinds that own one", "[reclass][ui]")
{
    application();

    Project project;
    project.add_class("Player", "app+0x10");
    auto& player = project.classes().front();
    player.nodes.push_back(node_of(project, NodeKind::utf16_text, "title"));
    player.nodes.push_back(array_of(project, 3, NodeKind::int32));
    player.nodes.push_back(node_of(project, NodeKind::int32, "health"));
    player.nodes.push_back(node_of(project, NodeKind::bitfield, "flags"));
    player.nodes.push_back(node_of(project, NodeKind::enum_node, "state"));

    Model model {project};

    // The run is the text the cell already draws, and the editor prefills the
    // bare number without its brackets.
    const QModelIndex text_run = model.index(0, Model::type);
    CHECK(model.data(text_run, Qt::DisplayRole).toString() == QStringLiteral("Utf16Text [16]"));
    CHECK(model.data(text_run, kExtentTextRole).toString() == QStringLiteral("[16]"));
    CHECK(model.data(text_run, Qt::EditRole).toString() == QStringLiteral("16"));

    const QModelIndex array_run = model.index(1, Model::type);
    CHECK(model.data(array_run, Qt::DisplayRole).toString() == QStringLiteral("Array<Int32> [3] ◀ (0) ▶"));
    CHECK(model.data(array_run, kExtentTextRole).toString() == QStringLiteral("[3]"));
    CHECK(model.data(array_run, Qt::EditRole).toString() == QStringLiteral("3"));

    // A bit field's width and an enum's name are not a number the user types,
    // and a plain scalar has no per-kind text at all.
    for (const int row : {2, 3, 4})
    {
        CHECK_FALSE(model.data(model.index(row, Model::type), kExtentTextRole).isValid());
        CHECK_FALSE(model.data(model.index(row, Model::type), Qt::EditRole).isValid());
    }
}

TEST_CASE("a hidden placeholder row offers no bracketed run", "[reclass][ui]")
{
    application();

    Project               project;
    ReclassStructureModel model {project};
    project.add_class("Player", "app+0x10");
    auto& player = project.classes().front();
    player.nodes.push_back(node_of(project, NodeKind::utf16_text, "title"));
    model.set_project(project);

    CHECK(model.data(model.index(0, Model::type), kExtentTextRole).toString() == QStringLiteral("[16]"));

    player.nodes[0]->hidden = true;
    model.rebuild();

    const auto* hidden = model.row_at({}, 0);
    REQUIRE(hidden != nullptr);
    REQUIRE(hidden->hidden);
    CHECK_FALSE(model.data(model.index(0, Model::type), kExtentTextRole).isValid());
    CHECK_FALSE(model.data(model.index(0, Model::type), Qt::EditRole).isValid());
    CHECK_FALSE(model.set_node_extent(*hidden, QStringLiteral("8")));
    CHECK(player.nodes[0]->extent() == 16);
}

TEST_CASE("a text node's length is edited in place through the model", "[reclass][ui]")
{
    application();

    Project project;
    project.add_class("Player", "app+0x10");
    auto& player = project.classes().front();
    player.nodes.push_back(node_of(project, NodeKind::utf16_text, "title"));
    player.nodes.push_back(node_of(project, NodeKind::int32, "health"));

    Model model {project};

    std::vector<QString> messages;
    QObject::connect(&model,
                     &ReclassStructureModel::statusChanged,
                     [&messages](const QString& message, bool)
                     {
                         messages.push_back(message);
                     });

    CHECK(model.set_node_extent(*model.row_at({}, 0), QStringLiteral("  32  ")));
    CHECK(player.nodes[0]->extent() == 32);
    CHECK(cell(model, model.index(0, 0), Model::type) == QStringLiteral("Utf16Text [32]"));
    // 32 utf16 characters are 64 bytes, so the row behind the text moves with
    // it and the int32 now starts at 0x40.
    CHECK(cell(model, model.index(1, 0), Model::offset) == QStringLiteral("40"));
    REQUIRE(messages.size() == 1);
    CHECK(messages.back() == QStringLiteral("Length set to 32."));

    // A typo is refused with its reason and the node keeps its length; a
    // refusal never re-flattens the class.
    for (const QString& bad :
         {QStringLiteral("0"), QStringLiteral(""), QStringLiteral("-1"), QStringLiteral("abc"), QStringLiteral("16x")})
    {
        const auto* current = model.row_at({}, 0);
        REQUIRE(current != nullptr);
        CHECK_FALSE(model.set_node_extent(*current, bad));
        CHECK(player.nodes[0]->extent() == 32);
        CHECK(messages.back() == QStringLiteral("Length: expected a whole number of at least 1."));
    }
}

TEST_CASE("an array's count is edited in place through the model", "[reclass][ui]")
{
    application();

    Project project;
    project.add_class("Player", "app+0x10");
    auto& player = project.classes().front();
    player.nodes.push_back(array_of(project, 2, NodeKind::int32));
    player.nodes.push_back(node_of(project, NodeKind::int32, "tail"));

    Model model {project};

    std::vector<QString> messages;
    QObject::connect(&model,
                     &ReclassStructureModel::statusChanged,
                     [&messages](const QString& message, bool)
                     {
                         messages.push_back(message);
                     });

    // An expanded array keeps its single element row as the count grows.
    model.set_expanded(player.nodes[0]->id, true);
    REQUIRE(model.rowCount(model.index(0, 0)) == 1);
    CHECK(cell(model, model.index(0, 0, model.index(0, 0)), Model::offset) == QStringLiteral("0"));
    CHECK(cell(model, model.index(1, 0), Model::offset) == QStringLiteral("8"));

    CHECK(model.set_node_extent(*model.row_at({}, 0), QStringLiteral("4")));
    REQUIRE(model.rowCount(model.index(0, 0)) == 1);
    CHECK(cell(model, model.index(0, 0), Model::type) == QStringLiteral("Array<Int32> [4] ◀ (0) ▶"));
    CHECK(cell(model, model.index(1, 0), Model::offset) == QStringLiteral("10"));
    REQUIRE(messages.size() == 1);
    CHECK(messages.back() == QStringLiteral("Count set to 4."));

    const auto* row = model.row_at({}, 0);
    REQUIRE(row != nullptr);
    CHECK_FALSE(model.set_node_extent(*row, QStringLiteral("0")));
    CHECK(messages.back() == QStringLiteral("Count: expected a whole number of at least 1."));
}

TEST_CASE("a row whose kind owns no number takes no edit", "[reclass][ui]")
{
    application();

    Project               project;
    ReclassStructureModel model {project};
    project.add_class("Player", "app+0x10");
    auto& player = project.classes().front();
    player.nodes.push_back(node_of(project, NodeKind::int32, "health"));
    player.nodes.push_back(node_of(project, NodeKind::bitfield, "flags"));
    model.set_project(project);

    for (const std::size_t index : {std::size_t {0}, std::size_t {1}})
    {
        const auto* row = model.row_at({}, index);
        REQUIRE(row != nullptr);
        CHECK_FALSE(model.set_node_extent(*row, QStringLiteral("4")));
    }
}

TEST_CASE("an array's element is stepped round both ends", "[reclass][ui]")
{
    application();

    Project project;
    project.add_class("Player", "app+0x10");
    auto& player = project.classes().front();
    player.nodes.push_back(array_of(project, 8, NodeKind::uint8));

    Model             model {project};
    const std::size_t array_id = player.nodes[0]->id;

    CHECK(model.element_count(*player.nodes[0]) == 8);
    CHECK(model.element_index(array_id) == 0);

    // The step before the first element is the last one.
    REQUIRE(model.step_element(array_id, -1));
    CHECK(model.element_index(array_id) == 7);
    CHECK(cell(model, model.index(0, 0), Model::type) == QStringLiteral("Array<UInt8> [8] ◀ (7) ▶"));

    // And the step past the last is the first.
    REQUIRE(model.step_element(array_id, 1));
    CHECK(model.element_index(array_id) == 0);

    // A step longer than the array keeps wrapping, and so does an index handed
    // over straight, which is what the inline editor does.
    REQUIRE(model.step_element(array_id, -9));
    CHECK(model.element_index(array_id) == 7);
    REQUIRE(model.set_element_index(array_id, 10));
    CHECK(model.element_index(array_id) == 2);

    // Writing the index the row already shows moves nothing and is accepted.
    CHECK(model.set_element_index(array_id, 2));

    // A node that is not an array has no element to walk.
    player.nodes.push_back(node_of(project, NodeKind::int32, "health"));
    model.rebuild();
    CHECK(model.element_count(*player.nodes[1]) == 0);
    CHECK(model.element_index(player.nodes[1]->id) == 0);
    CHECK_FALSE(model.step_element(player.nodes[1]->id, 1));
    CHECK_FALSE(model.set_element_index(player.nodes[1]->id, 2));
}

TEST_CASE("an array's one element has nowhere to step", "[reclass][ui]")
{
    application();

    Project project;
    project.add_class("Player", "app+0x10");
    auto& player = project.classes().front();
    player.nodes.push_back(array_of(project, 1, NodeKind::uint8));

    Model             model {project};
    const std::size_t array_id = player.nodes[0]->id;

    // The control is drawn for a single element, but an arrow click is a no-op.
    CHECK(model.element_count(*player.nodes[0]) == 1);
    CHECK(cell(model, model.index(0, 0), Model::type) == QStringLiteral("Array<UInt8> [1] ◀ (0) ▶"));
    CHECK_FALSE(model.step_element(array_id, 1));
    CHECK_FALSE(model.step_element(array_id, -1));
    CHECK(model.element_index(array_id) == 0);

    model.set_expanded(array_id, true);
    REQUIRE(model.rowCount(model.index(0, 0)) == 1);
}

TEST_CASE("an array with no element draws no index control", "[reclass][ui]")
{
    application();

    Project project;
    project.add_class("Player", "app+0x10");
    auto& player = project.classes().front();
    player.nodes.push_back(array_of(project, 0, NodeKind::uint8));

    Model             model {project};
    const std::size_t array_id = player.nodes[0]->id;

    CHECK(model.element_count(*player.nodes[0]) == 0);
    CHECK(cell(model, model.index(0, 0), Model::type) == QStringLiteral("Array [0]"));
    CHECK(model.element_runs(array_id).previous == -1);
    CHECK_FALSE(model.step_element(array_id, 1));
    CHECK_FALSE(model.set_element_index(array_id, 0));

    model.set_expanded(array_id, true);
    CHECK(model.rowCount(model.index(0, 0)) == 0);
}

TEST_CASE("an array's index control answers where its arrows and digits sit", "[reclass][ui]")
{
    application();

    Project project;
    project.add_class("Player", "app+0x10");
    auto& player = project.classes().front();
    player.nodes.push_back(array_of(project, 12, NodeKind::uint8));
    player.nodes.push_back(node_of(project, NodeKind::uint32, "health"));

    Model             model {project};
    const std::size_t array_id = player.nodes[0]->id;

    REQUIRE(model.set_element_index(array_id, 2));
    const QString one_digit = cell(model, model.index(0, 0), Model::type);
    REQUIRE(one_digit == QStringLiteral("Array<UInt8> [12] ◀ (2) ▶"));

    // The control sits at the end of the text the cell draws: the arrow, the
    // bracketed index the editor takes over, then the other arrow.
    const ElementRuns runs = model.element_runs(array_id);
    CHECK(runs.previous == one_digit.indexOf(QChar(0x25C0)));
    CHECK(runs.index == one_digit.indexOf(QChar(u'(')) + 1);
    CHECK(runs.index_length == 1);
    CHECK(runs.next == one_digit.size() - 1);
    CHECK(one_digit.mid(runs.previous, 1) == QStringLiteral("◀"));
    CHECK(one_digit.mid(runs.index, runs.index_length) == QStringLiteral("2"));
    CHECK(one_digit.mid(runs.next, 1) == QStringLiteral("▶"));

    // A two-digit index widens the digit run the editor takes over.
    REQUIRE(model.set_element_index(array_id, 10));
    const QString two_digits = cell(model, model.index(0, 0), Model::type);
    REQUIRE(two_digits == QStringLiteral("Array<UInt8> [12] ◀ (10) ▶"));
    const ElementRuns wide = model.element_runs(array_id);
    CHECK(wide.index_length == 2);
    CHECK(two_digits.mid(wide.index, wide.index_length) == QStringLiteral("10"));
    CHECK(two_digits.size() == one_digit.size() + 1);

    // A column that draws no control answers no runs at all.
    CHECK(model.element_runs(player.nodes[1]->id).previous == -1);
    CHECK(model.element_runs(player.nodes[1]->id).index == -1);
}

TEST_CASE("a class array keeps its class and gains the index control", "[reclass][ui]")
{
    application();

    Project project;
    project.add_class("Point", "app+0x20");
    project.classes()[0].nodes.push_back(node_of(project, NodeKind::int32, "x"));
    project.add_class("Player", "app+0x10");
    auto& player = project.classes()[1];
    player.nodes.push_back(wrapper_array_of(project, "Point", 3));
    player.nodes.push_back(instance_array_of(project, "Point", 4));

    Model model {project};
    model.set_class(1);

    CHECK(cell(model, model.index(0, 0), Model::type) == QStringLiteral("Class Array <Point> [3] ◀ (0) ▶"));
    CHECK(cell(model, model.index(1, 0), Model::type) == QStringLiteral("Class Instance Array <Point> [4] ◀ (0) ▶"));
    CHECK(model.element_count(*player.nodes[0]) == 3);
    CHECK(model.element_count(*player.nodes[1]) == 4);

    // The two class arrays are walked exactly like a plain array.
    REQUIRE(model.step_element(player.nodes[1]->id, 1));
    CHECK(cell(model, model.index(1, 0), Model::type) == QStringLiteral("Class Instance Array <Point> [4] ◀ (1) ▶"));
}

TEST_CASE("the index is clamped to a shrunk count and dropped with the kind", "[reclass][ui]")
{
    application();

    Project project;
    project.add_class("Player", "app+0x10");
    auto& player = project.classes().front();
    player.nodes.push_back(array_of(project, 8, NodeKind::uint8));

    Model             model {project};
    const std::size_t array_id = player.nodes[0]->id;

    REQUIRE(model.set_element_index(array_id, 5));
    CHECK(model.element_index(array_id) == 5);

    // A count the user shrinks clamps the index to the new last element.
    REQUIRE(model.set_node_extent(*model.row_at({}, 0), QStringLiteral("3")));
    CHECK(model.element_count(*player.nodes[0]) == 3);
    CHECK(model.element_index(array_id) == 2);
    CHECK(cell(model, model.index(0, 0), Model::type) == QStringLiteral("Array<UInt8> [3] ◀ (2) ▶"));

    // A node re-typed away from an array keeps no index at all.
    slopkit::reclass::change_node_kind(project, player.nodes, 0, NodeKind::int32);
    model.rebuild();
    CHECK(model.element_count(*player.nodes[0]) == 0);
    CHECK(model.element_index(array_id) == 0);
}

TEST_CASE("the index returns to the first element when the class changes", "[reclass][ui]")
{
    application();

    Project project;
    project.add_class("Point", "app+0x20");
    project.classes()[0].nodes.push_back(array_of(project, 4, NodeKind::uint8));
    project.add_class("Player", "app+0x10");
    project.classes()[1].nodes.push_back(array_of(project, 4, NodeKind::uint8));

    Model             model {project};
    const std::size_t point_array = project.classes()[0].nodes[0]->id;

    model.set_class(1);
    const std::size_t player_array = project.classes()[1].nodes[0]->id;
    REQUIRE(model.set_element_index(player_array, 3));
    CHECK(model.element_index(player_array) == 3);

    // Switching the class and back drops both the walk and the chosen element,
    // exactly as it drops a node's expansion.
    model.set_class(0);
    CHECK(model.element_index(point_array) == 0);
    CHECK_FALSE(model.is_expanded(point_array));
    model.set_class(1);
    CHECK(model.element_index(player_array) == 0);
}
