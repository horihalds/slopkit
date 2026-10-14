#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QAction>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMenu>
#include <QMouseEvent>

#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "reclass/node.hpp"
#include "reclass/project.hpp"
#include "support/reclass_helpers.hpp"
#include "support/reclass_live_helpers.hpp"
#include "support/ui_helpers.hpp"
#include "ui/components/project_view.hpp"
#include "ui/components/reclass_view.hpp"
#include "ui/dialogs/reclass.hpp"
#include "ui/models/reclass_proxy_model.hpp"
#include "ui/models/reclass_structure_model.hpp"

namespace
{
    using slopkit::process::AccessWorker;
    using slopkit::process::AttachedTarget;
    using slopkit::reclass::Class;
    using slopkit::reclass::NodeKind;
    using slopkit::reclass::Project;
    using slopkit::test::attach_session;
    using slopkit::test::drain_until_idle;
    using slopkit::test::FakeAccess;
    using slopkit::test::module_image;
    using slopkit::test::node_of;
    using slopkit::test::reading_for;
    using slopkit::test::stage_bytes;
    using slopkit::ui::dialogs::ReClassDialog;

    // Point { int32 x; int32 y; } and Player { int8 tag; pointer next; }, with a
    // worker that is never asked for anything in this step.
    class Fixture
    {
    public:
        Fixture()
        {
            project.add_class("Point", "app+0x20");
            project.add_class("Player", "app+0x10");

            Class& point = project.classes()[0];
            point.nodes.push_back(node_of(project, NodeKind::int32, "x"));
            point.nodes.push_back(node_of(project, NodeKind::int32, "y"));

            Class& player = project.classes()[1];
            player.nodes.push_back(node_of(project, NodeKind::int8, "tag"));
            player.nodes.push_back(node_of(project, NodeKind::pointer, "next"));
        }

        Project        project;
        FakeAccess     access;
        AccessWorker   worker {access};
        AttachedTarget target;
        ReClassDialog* dialog {};
    };

    void select_player(Fixture& fixture)
    {
        fixture.dialog->project_view()->select_class(1);
        QCoreApplication::processEvents();
        REQUIRE(fixture.dialog->structure()->selected_class() == 1);
    }

    constexpr std::uint64_t kAppBase = 0x400000;

    // One live pass through the window's own surface: the reads the window asks
    // for, answered the way the live values pass answers them.
    void live_pass(Fixture& fixture, ReClassDialog& dialog)
    {
        const std::vector<slopkit::ui::LiveRequest> requests = dialog.next_live_request();
        drain_until_idle(fixture.worker);

        std::vector<slopkit::ui::LiveReading> readings;
        readings.reserve(requests.size());
        for (const slopkit::ui::LiveRequest& request : requests)
        {
            readings.push_back(reading_for(fixture.access, request.id, request.address, request.size));
        }
        dialog.apply_live_readings(readings);
    }
}

TEST_CASE("the ReClass window opens as a companion window", "[reclass][ui]")
{
    application();

    Fixture       fixture;
    ReClassDialog dialog {fixture.project, fixture.worker, fixture.target};
    fixture.dialog = &dialog;

    // A companion window: no parent widget keeps it out of the main window's
    // stacking, and it must not own the process lifetime.
    CHECK(dialog.parentWidget() == nullptr);
    CHECK(dialog.isWindow());
    CHECK_FALSE(dialog.testAttribute(Qt::WA_QuitOnClose));
    CHECK(dialog.windowTitle() == QStringLiteral("ReClass"));

    dialog.show();
    QCoreApplication::processEvents();

    REQUIRE(dialog.project_view()->class_count() == 2);
    CHECK(dialog.project_view()->class_name(0) == QStringLiteral("Point"));
    CHECK(dialog.project_view()->class_name(1) == QStringLiteral("Player"));
    CHECK(dialog.project_view()->selected_class() == std::optional<std::size_t> {0});
    CHECK(dialog.structure()->valid_class());
    CHECK(dialog.structure()->rowCount() == 2);
    CHECK(dialog.grid()->model() == dialog.proxy());

    // The grid is painted by its own component rather than assembled out of a
    // tree view: the six columns are sized by the component to the text the class
    // draws, no header band sits above them, and nothing about the layout can be
    // dragged.
    // Only the value column answers a resize, so a reading keeps fitting while
    // the columns the user reads by eye stay where they are.
    using slopkit::ui::models::ReclassStructureModel;
    CHECK(dialog.grid()->row_count() == 2);
    CHECK(dialog.grid()->column_x(ReclassStructureModel::comment)
          == dialog.grid()->column_x(ReclassStructureModel::value)
                 + dialog.grid()->column_width(ReclassStructureModel::value));

    const int address_width = dialog.grid()->column_width(ReclassStructureModel::address);
    const int value_width   = dialog.grid()->column_width(ReclassStructureModel::value);
    dialog.resize(dialog.width() + 160, dialog.height());
    QCoreApplication::processEvents();

    CHECK(dialog.grid()->column_width(ReclassStructureModel::address) == address_width);
    CHECK(dialog.grid()->column_width(ReclassStructureModel::value) > value_width);

    // Re-opening brings the same window and the same project back.
    dialog.hide();
    dialog.show();
    QCoreApplication::processEvents();

    CHECK(dialog.project_view()->class_count() == 2);
    CHECK(dialog.structure()->rowCount() == 2);

    dialog.close();
}

TEST_CASE("the class commands add, rename, re-address and delete", "[reclass][ui]")
{
    application();

    Fixture       fixture;
    ReClassDialog dialog {fixture.project, fixture.worker, fixture.target};
    fixture.dialog = &dialog;
    dialog.show();
    QCoreApplication::processEvents();

    REQUIRE(dialog.create_class(QStringLiteral("Enemy"), QStringLiteral("app+0x30")));
    CHECK(fixture.project.classes().size() == 3);
    CHECK(dialog.project_view()->class_count() == 3);
    CHECK(dialog.project_view()->class_name(2) == QStringLiteral("Enemy"));
    CHECK(dialog.structure()->selected_class() == 2);
    CHECK_FALSE(dialog.status_label()->text().isEmpty());

    // A new class arrives covering 1024 bytes, so the grid starts as a page of
    // the target the user retypes instead of an empty list.
    CHECK(fixture.project.classes()[2].nodes.size() == 128);
    CHECK(fixture.project.classes()[2].nodes.front()->kind() == NodeKind::hex64);
    CHECK(dialog.structure()->rowCount() == 128);

    // A duplicate name, an empty name and a malformed expression are refused.
    CHECK_FALSE(dialog.create_class(QStringLiteral("Enemy"), QStringLiteral("app+0x40")));
    CHECK(fixture.project.classes().size() == 3);
    CHECK_FALSE(dialog.create_class(QStringLiteral("  "), QStringLiteral("app+0x40")));
    CHECK_FALSE(dialog.create_class(QStringLiteral("Other"), QStringLiteral("app+")));
    CHECK_FALSE(dialog.create_class(QStringLiteral("Other"), QString()));
    CHECK(fixture.project.classes().size() == 3);

    CHECK(dialog.rename_selected_class(QStringLiteral("Boss")));
    CHECK(dialog.project_view()->class_name(2) == QStringLiteral("Boss"));
    CHECK_FALSE(dialog.rename_selected_class(QStringLiteral("Player")));
    CHECK(dialog.project_view()->class_name(2) == QStringLiteral("Boss"));

    CHECK(dialog.set_selected_class_address(QStringLiteral("game.exe+0x99")));
    CHECK(fixture.project.classes()[2].address_expression == "game.exe+0x99");
    CHECK_FALSE(dialog.set_selected_class_address(QStringLiteral("app+")));
    CHECK(fixture.project.classes()[2].address_expression == "game.exe+0x99");

    dialog.delete_class();
    CHECK(fixture.project.classes().size() == 2);
    CHECK(dialog.project_view()->class_count() == 2);

    dialog.close();
}

TEST_CASE("the node commands build and edit the displayed class", "[reclass][ui]")
{
    application();

    Fixture       fixture;
    ReClassDialog dialog {fixture.project, fixture.worker, fixture.target};
    fixture.dialog = &dialog;
    dialog.show();
    QCoreApplication::processEvents();
    select_player(fixture);

    REQUIRE(dialog.structure()->rowCount() == 2);
    Class& player = fixture.project.classes()[1];

    // Inserting with nothing selected appends to the end of the class.
    dialog.grid()->setCurrentIndex({});
    CHECK_FALSE(dialog.delete_node_action()->isEnabled());
    CHECK_FALSE(dialog.follow_action()->isEnabled());
    dialog.insert_node(NodeKind::int32);
    REQUIRE(player.nodes.size() == 3);
    CHECK(player.nodes[2]->kind() == NodeKind::int32);
    CHECK(player.nodes[2]->name.empty());

    // Inserting with a row selected puts the node before it, and selecting a row
    // is what makes the commands that act on it available.
    dialog.grid()->setCurrentIndex(dialog.structure()->index(1, 0));
    CHECK(dialog.delete_node_action()->isEnabled());
    CHECK(dialog.follow_action()->isEnabled());
    dialog.insert_node(NodeKind::float32);
    REQUIRE(player.nodes.size() == 4);
    CHECK(player.nodes[1]->kind() == NodeKind::float32);
    CHECK(player.nodes[1]->name.empty());

    // Shrinking a node's type pads the freed bytes so offsets stay put.
    dialog.grid()->setCurrentIndex(dialog.structure()->index(1, 0));
    dialog.change_node_kind(NodeKind::int8);
    REQUIRE(player.nodes.size() == 6);
    CHECK(player.nodes[1]->kind() == NodeKind::int8);
    CHECK(player.nodes[2]->kind() == NodeKind::hex16);
    CHECK(player.nodes[3]->kind() == NodeKind::hex8);
    CHECK(player.nodes[3]->name.empty());

    // Show as hex acts on the selection, and Delete removes it.
    dialog.grid()->setCurrentIndex(dialog.structure()->index(1, 0));
    dialog.set_node_hex(true);
    CHECK(player.nodes[1]->hex);
    dialog.set_node_hex(false);
    CHECK_FALSE(player.nodes[1]->hex);

    dialog.copy_name();
    CHECK(dialog.status_label()->text() == QStringLiteral("Name copied."));

    // A name typed into the grid's own cell goes through the structure model,
    // and the window reports the outcome on the same status line as a write.
    using slopkit::ui::models::ReclassStructureModel;
    const QModelIndex name_cell = dialog.proxy()->index(0, ReclassStructureModel::name);
    CHECK(dialog.proxy()->flags(name_cell).testFlag(Qt::ItemIsEditable));
    CHECK(dialog.proxy()->setData(name_cell, QStringLiteral("vitals"), Qt::EditRole));
    CHECK(player.nodes[0]->name == "vitals");
    CHECK(dialog.status_label()->text() == QStringLiteral("Node renamed."));

    CHECK_FALSE(dialog.proxy()->setData(name_cell, QStringLiteral("   "), Qt::EditRole));
    CHECK(player.nodes[0]->name == "vitals");
    CHECK(dialog.status_label()->text() == QStringLiteral("A node name is required."));

    dialog.grid()->setCurrentIndex(dialog.structure()->index(1, 0));
    dialog.remove_selected_nodes();
    CHECK(player.nodes.size() == 5);
    CHECK(player.nodes[1]->kind() == NodeKind::hex16);

    CHECK(dialog.structure()->rowCount() == 5);
    dialog.close();
}

TEST_CASE("the window redraws the project it is handed after a table load", "[reclass][ui]")
{
    application();

    Fixture       fixture;
    ReClassDialog dialog {fixture.project, fixture.worker, fixture.target};
    fixture.dialog = &dialog;
    dialog.show();
    QCoreApplication::processEvents();

    REQUIRE(dialog.project_view()->class_count() == 2);

    fixture.project.clear();
    fixture.project.add_class("Only", "app+0x10");
    dialog.refresh_project();

    REQUIRE(dialog.project_view()->class_count() == 1);
    CHECK(dialog.project_view()->class_name(0) == QStringLiteral("Only"));
    CHECK(dialog.structure()->valid_class());
    CHECK(dialog.structure()->rowCount() == 0);

    dialog.close();
}

TEST_CASE("dissect guesses the kind a hex node's live bytes fit", "[reclass][ui]")
{
    application();

    Fixture       fixture;
    ReClassDialog dialog {fixture.project, fixture.worker, fixture.target};
    fixture.dialog = &dialog;
    fixture.target = slopkit::test::fake_target();
    attach_session(fixture.worker);
    dialog.set_modules(std::vector<slopkit::process::ModuleInfo> {module_image("app", kAppBase, 0x1000)});
    dialog.show();
    QCoreApplication::processEvents();

    // A hex64 node over the 8 bytes after Point's own fields, so that its
    // offset is 4-byte aligned.
    Class& point = fixture.project.classes()[0];
    point.nodes.push_back(node_of(fixture.project, NodeKind::hex64, "raw"));
    dialog.refresh_project();
    dialog.grid()->setCurrentIndex(dialog.structure()->index(2, 0));

    // A value inside the module image is a pointer without any probe.
    const std::uint64_t raw_address = kAppBase + 0x20 + 8;
    stage_bytes(fixture.access, raw_address, {0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00});
    live_pass(fixture, dialog);
    live_pass(fixture, dialog);

    REQUIRE(point.nodes[2]->kind() == NodeKind::hex64);

    // The header and the class tooltips spell a resolved address the way the
    // address column does: absolute, zero-padded to the pointer width.
    CHECK(dialog.header_label()->text() == QStringLiteral("Point · app+0x20 → 0x0000000000400020"));
    CHECK(dialog.project_view()->class_item(0)->toolTip(0) == QStringLiteral("app+0x20 → 0x0000000000400020"));

    dialog.dissect();
    drain_until_idle(fixture.worker);

    CHECK(point.nodes[2]->kind() == NodeKind::pointer);
    CHECK(dialog.status_label()->text() == QStringLiteral("dissected 1 of 1 nodes"));

    // Without a selected hex node whose bytes were read there is nothing to do.
    dialog.dissect();
    CHECK(dialog.status_label()->text().startsWith(QStringLiteral("Select a hex node")));

    dialog.close();
}

TEST_CASE("follow pointer references a class and inlines it", "[reclass][ui]")
{
    application();

    Fixture       fixture;
    ReClassDialog dialog {fixture.project, fixture.worker, fixture.target};
    fixture.dialog = &dialog;
    dialog.show();
    QCoreApplication::processEvents();
    select_player(fixture);

    Class& player = fixture.project.classes()[1];
    REQUIRE(player.nodes[1]->kind() == NodeKind::pointer);

    dialog.grid()->setCurrentIndex(dialog.structure()->index(1, 0));
    dialog.set_followed_class(QStringLiteral("Point"));

    CHECK(player.nodes[1]->kind() == NodeKind::class_pointer);
    CHECK(player.nodes[1]->class_name == "Point");
    CHECK(dialog.structure()->is_expanded(player.nodes[1]->id));
    CHECK(dialog.structure()->rowCount(dialog.structure()->index(1, 0)) == 2);
    CHECK(dialog.status_label()->text() == QStringLiteral("Following 'Point'."));

    dialog.grid()->setCurrentIndex(dialog.structure()->index(1, 0));
    dialog.set_followed_class(QString());
    CHECK(player.nodes[1]->kind() == NodeKind::pointer);
    CHECK(player.nodes[1]->class_name.empty());
    CHECK(dialog.structure()->rowCount(dialog.structure()->index(1, 0)) == 0);

    dialog.set_followed_class(QStringLiteral("Nobody"));
    CHECK(player.nodes[1]->kind() == NodeKind::pointer);
    CHECK(dialog.status_label()->text() == QStringLiteral("Unknown class 'Nobody'."));

    // A node that is not a pointer cannot be followed.
    dialog.grid()->setCurrentIndex(dialog.structure()->index(0, 0));
    dialog.set_followed_class(QStringLiteral("Point"));
    CHECK(player.nodes[0]->kind() == NodeKind::int8);
    CHECK(dialog.status_label()->text() == QStringLiteral("Select a pointer node first."));

    dialog.close();
}

TEST_CASE("the ReClass window carries the reference's shell", "[reclass][ui]")
{
    application();

    Fixture       fixture;
    ReClassDialog dialog {fixture.project, fixture.worker, fixture.target};
    fixture.dialog = &dialog;

    REQUIRE(dialog.menu_bar() != nullptr);
    REQUIRE(dialog.tool_bar() != nullptr);
    REQUIRE(dialog.status_bar() != nullptr);

    // The menu strip holds the reference's View, Node and Project menus, and the
    // toolbar the reference's commands.
    const QStringList menus = [&dialog]
    {
        QStringList titles;
        for (QAction* action : dialog.menu_bar()->actions())
        {
            titles << action->text();
        }
        return titles;
    }();
    CHECK(menus.contains(QStringLiteral("View")));
    CHECK(menus.contains(QStringLiteral("Node")));
    CHECK(menus.contains(QStringLiteral("Project")));

    CHECK(dialog.new_class_action() != nullptr);
    CHECK(dialog.delete_class_action() != nullptr);
    CHECK(dialog.delete_node_action() != nullptr);
    CHECK(dialog.dissect_action() != nullptr);
    CHECK(dialog.follow_action() != nullptr);
    CHECK(dialog.hierarchy_action() != nullptr);

    // The node-kind button and both byte buttons carry their grouped menus.
    REQUIRE(dialog.add_node_button() != nullptr);
    REQUIRE(dialog.add_node_button()->menu() != nullptr);
    CHECK(dialog.add_node_button()->menu()->actions().size() > 3);

    auto* add_bytes    = dialog.tool_bar()->findChild<QToolButton*>(QStringLiteral("reclass_add_bytes"));
    auto* insert_bytes = dialog.tool_bar()->findChild<QToolButton*>(QStringLiteral("reclass_insert_bytes"));
    REQUIRE(add_bytes != nullptr);
    REQUIRE(insert_bytes != nullptr);
    REQUIRE(add_bytes->menu() != nullptr);
    REQUIRE(insert_bytes->menu() != nullptr);
    // The reference's seven presets and the "X bytes…" prompt.
    CHECK(add_bytes->menu()->actions().size() >= 8);

    // The hierarchy view is the window's toggle, and the tree follows it.
    CHECK_FALSE(dialog.class_hierarchy_view());
    dialog.set_class_hierarchy_view(true);
    CHECK(dialog.class_hierarchy_view());
    CHECK(dialog.hierarchy_action()->isChecked());
    dialog.set_class_hierarchy_view(false);
    CHECK_FALSE(dialog.class_hierarchy_view());

    dialog.close();
}

TEST_CASE("adding and inserting bytes keeps the offsets of the following nodes", "[reclass][ui]")
{
    application();

    Fixture       fixture;
    ReClassDialog dialog {fixture.project, fixture.worker, fixture.target};
    fixture.dialog = &dialog;
    dialog.show();
    select_player(fixture);

    Class& player = fixture.project.classes()[1];
    REQUIRE(player.nodes.size() == 2);

    // Adding bytes appends behind the last node, so the nodes that were there
    // keep their offsets.
    CHECK(dialog.add_bytes(4));
    REQUIRE(player.nodes.size() == 3);
    CHECK(dialog.structure()->row_for(dialog.structure()->index(1, 0))->offset == 1);

    // Inserting in front of the selection pushes the selection down.
    dialog.grid()->setCurrentIndex(dialog.structure()->index(0, 0));
    CHECK(dialog.insert_bytes(4));
    REQUIRE(player.nodes.size() == 4);
    CHECK(player.nodes[0]->kind() == NodeKind::hex32);
    CHECK(player.nodes[1]->kind() == NodeKind::int8);
    CHECK(dialog.structure()->row_for(dialog.structure()->index(2, 0))->offset == 5);

    // A zero count changes nothing.
    CHECK_FALSE(dialog.add_bytes(0));
    CHECK_FALSE(dialog.insert_bytes(0));
    CHECK(player.nodes.size() == 4);

    dialog.close();
}

TEST_CASE("the row menu offers both byte commands", "[reclass][ui]")
{
    application();

    Fixture       fixture;
    ReClassDialog dialog {fixture.project, fixture.worker, fixture.target};
    fixture.dialog = &dialog;
    dialog.show();
    QCoreApplication::processEvents();
    select_player(fixture);

    const auto submenu = [](QMenu& menu, const QString& title) -> QMenu*
    {
        for (QAction* action : menu.actions())
        {
            if (action->text() == title)
            {
                return action->menu();
            }
        }
        return nullptr;
    };
    const auto item = [](QMenu& menu, const QString& text) -> QAction*
    {
        for (QAction* action : menu.actions())
        {
            if (action->text() == text)
            {
                return action;
            }
        }
        return nullptr;
    };

    Class& player = fixture.project.classes()[1];
    REQUIRE(player.nodes.size() == 2);

    // Both byte commands sit in the node menu, each with the toolbar's seven
    // presets and its prompt.
    QMenu row_menu;
    dialog.populate_row_menu(row_menu);

    QMenu* add    = submenu(row_menu, QStringLiteral("Add Bytes"));
    QMenu* insert = submenu(row_menu, QStringLiteral("Insert Bytes"));
    REQUIRE(add != nullptr);
    REQUIRE(insert != nullptr);
    REQUIRE(add->actions().size() == 9);
    REQUIRE(insert->actions().size() == 9);
    CHECK(add->actions().front()->text() == QStringLiteral("Add 4 Bytes"));
    CHECK(add->actions().back()->text() == QStringLiteral("Add … Bytes"));
    CHECK(insert->actions().front()->text() == QStringLiteral("Insert 4 Bytes"));
    CHECK(item(*insert, QStringLiteral("Insert 2048 Bytes")) != nullptr);

    // The reference enables them only where they can act, which needs a node.
    CHECK_FALSE(add->isEnabled());
    CHECK_FALSE(insert->isEnabled());

    dialog.grid()->setCurrentIndex(dialog.structure()->index(0, 0));

    QMenu selected;
    dialog.populate_row_menu(selected);
    CHECK(submenu(selected, QStringLiteral("Add Bytes"))->isEnabled());
    CHECK(submenu(selected, QStringLiteral("Insert Bytes"))->isEnabled());

    // Inserting goes in front of the selected node, so it and the nodes after it
    // keep their offsets.
    item(*submenu(selected, QStringLiteral("Insert Bytes")), QStringLiteral("Insert 4 Bytes"))->trigger();
    REQUIRE(player.nodes.size() == 3);
    CHECK(player.nodes[0]->kind() == NodeKind::hex32);
    CHECK(player.nodes[1]->kind() == NodeKind::int8);
    CHECK(dialog.structure()->row_for(dialog.structure()->index(2, 0))->offset == 5);

    // Adding appends behind the selection's container, so it lands at the end.
    QMenu appended;
    dialog.populate_row_menu(appended);
    item(*submenu(appended, QStringLiteral("Add Bytes")), QStringLiteral("Add 8 Bytes"))->trigger();
    REQUIRE(player.nodes.size() == 4);
    CHECK(player.nodes.back()->kind() == NodeKind::hex64);

    // A multiple selection offers no single insertion point, so only adding is
    // left.
    const QModelIndex          index = dialog.structure()->index(dialog.structure()->rowCount() - 1, 0);
    const std::optional<QRect> rect  = dialog.grid()->cell_rect(index);
    REQUIRE(rect.has_value());
    const QPoint point  = rect->center();
    const QPoint global = dialog.grid()->viewport()->mapToGlobal(point);
    QMouseEvent  press {
        QEvent::MouseButtonPress, QPointF(point), QPointF(global), Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier};
    QApplication::sendEvent(dialog.grid()->viewport(), &press);
    REQUIRE(dialog.grid()->selected_rows().size() == 2);

    QMenu multiple;
    dialog.populate_row_menu(multiple);
    CHECK(submenu(multiple, QStringLiteral("Add Bytes"))->isEnabled());
    CHECK_FALSE(submenu(multiple, QStringLiteral("Insert Bytes"))->isEnabled());

    dialog.close();
}

TEST_CASE("hiding a node collapses it without moving the following nodes", "[reclass][ui]")
{
    application();

    Fixture       fixture;
    ReClassDialog dialog {fixture.project, fixture.worker, fixture.target};
    fixture.dialog = &dialog;
    dialog.show();
    select_player(fixture);

    Class& player = fixture.project.classes()[1];
    REQUIRE(player.nodes.size() == 2);

    // Player { int8 tag; pointer next; }: hiding the pointer keeps its bytes.
    dialog.grid()->setCurrentIndex(dialog.structure()->index(1, 0));
    dialog.hide_selected_nodes();
    CHECK(player.nodes[1]->hidden);
    CHECK(dialog.status_label()->text() == QStringLiteral("1 node(s) hidden."));

    const auto* hidden_row = dialog.structure()->row_for(dialog.structure()->index(1, 0));
    REQUIRE(hidden_row != nullptr);
    CHECK(hidden_row->hidden);
    CHECK_FALSE(dialog.structure()->expandable(*hidden_row));

    // Clicking the placeholder row asks for the node back.
    emit dialog.grid()->hiddenRowActivated(player.nodes[1]->id);
    CHECK_FALSE(player.nodes[1]->hidden);
    CHECK(dialog.status_label()->text() == QStringLiteral("Node revealed."));

    // Hide again, then reveal it through the menu command instead.
    dialog.grid()->setCurrentIndex(dialog.structure()->index(1, 0));
    dialog.hide_selected_nodes();
    CHECK(player.nodes[1]->hidden);
    dialog.unhide_selected_nodes();
    CHECK_FALSE(player.nodes[1]->hidden);

    // Unhide above/below reveals the run of hidden siblings on one side.
    player.nodes[0]->hidden = true;
    dialog.grid()->setCurrentIndex(dialog.structure()->index(1, 0));
    dialog.unhide_below_nodes();
    CHECK(player.nodes[1]->hidden == false);
    dialog.unhide_above_nodes();
    CHECK_FALSE(player.nodes[0]->hidden);

    dialog.close();
}

TEST_CASE("a copied node is pasted with a fresh id", "[reclass][ui]")
{
    application();

    Fixture       fixture;
    ReClassDialog dialog {fixture.project, fixture.worker, fixture.target};
    fixture.dialog = &dialog;
    dialog.show();
    select_player(fixture);

    Class& player = fixture.project.classes()[1];
    REQUIRE(player.nodes.size() == 2);
    const std::size_t tag_id = player.nodes[0]->id;

    dialog.grid()->setCurrentIndex(dialog.structure()->index(0, 0));
    dialog.copy_selected_nodes();
    CHECK(dialog.clipboard_size() == 1);
    CHECK(dialog.status_label()->text() == QStringLiteral("1 node(s) copied."));

    // Pasting behind the class appends the clone, whose id is new.
    CHECK_FALSE(dialog.grid()->currentIndex().isValid() == false);
    dialog.paste_nodes();
    REQUIRE(player.nodes.size() == 3);
    CHECK(player.nodes[2]->kind() == NodeKind::int8);
    CHECK(player.nodes[2]->id != tag_id);
    CHECK(player.nodes[2]->name == "tag");

    dialog.close();
}

TEST_CASE("create class from nodes moves the selection into a new class", "[reclass][ui]")
{
    application();

    Fixture       fixture;
    ReClassDialog dialog {fixture.project, fixture.worker, fixture.target};
    fixture.dialog = &dialog;
    dialog.show();
    select_player(fixture);

    Class& player = fixture.project.classes()[1];
    REQUIRE(player.nodes.size() == 2);
    REQUIRE(fixture.project.classes().size() == 2);

    dialog.grid()->setCurrentIndex(dialog.structure()->index(1, 0));
    dialog.create_class_from_nodes();

    // The pointer moved into a class of its own and a class instance takes its
    // place, so Player still covers the same bytes.
    REQUIRE(fixture.project.classes().size() == 3);
    const Class& created = fixture.project.classes()[2];
    REQUIRE(created.nodes.size() == 1);
    CHECK(created.nodes[0]->kind() == NodeKind::pointer);
    REQUIRE(player.nodes.size() == 2);
    CHECK(player.nodes[1]->kind() == NodeKind::class_instance);
    CHECK(player.nodes[1]->class_name == created.name);

    dialog.close();
}

TEST_CASE("shrink class trims the trailing padding", "[reclass][ui]")
{
    application();

    Fixture       fixture;
    ReClassDialog dialog {fixture.project, fixture.worker, fixture.target};
    fixture.dialog = &dialog;
    dialog.show();
    select_player(fixture);

    Class& player = fixture.project.classes()[1];
    REQUIRE(player.nodes.size() == 2);

    CHECK(dialog.add_bytes(4));
    REQUIRE(player.nodes.size() == 3);
    CHECK(player.nodes[2]->kind() == NodeKind::hex32);

    select_player(fixture);
    dialog.shrink_class();
    CHECK(player.nodes.size() == 2);
    CHECK(dialog.status_label()->text() == QStringLiteral("1 padding node(s) removed."));

    dialog.close();
}
