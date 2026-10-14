#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QHeaderView>
#include <QListWidget>
#include <QTreeView>

#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "reclass/node.hpp"
#include "reclass/project.hpp"
#include "support/reclass_helpers.hpp"
#include "support/reclass_live_helpers.hpp"
#include "support/ui_helpers.hpp"
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
        fixture.dialog->class_list()->setCurrentRow(1);
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

    REQUIRE(dialog.class_list()->count() == 2);
    CHECK(dialog.class_list()->item(0)->text() == QStringLiteral("Point"));
    CHECK(dialog.class_list()->item(1)->text() == QStringLiteral("Player"));
    CHECK(dialog.class_list()->currentRow() == 0);
    CHECK(dialog.structure()->valid_class());
    CHECK(dialog.structure()->rowCount() == 2);
    CHECK(dialog.grid()->model() == dialog.proxy());

    // The header may not measure the model: a ResizeToContents column re-reads
    // every row on each pass a reading moves, which is what made the grid
    // stutter on the live cadence. Only the value column stretches; the rest
    // hold the width their own text needs.
    CHECK(dialog.grid()->header()->sectionResizeMode(slopkit::ui::models::ReclassStructureModel::value)
          == QHeaderView::Stretch);
    CHECK(dialog.grid()->header()->sectionResizeMode(slopkit::ui::models::ReclassStructureModel::address)
          == QHeaderView::Interactive);
    CHECK(dialog.grid()->alternatingRowColors());

    // Re-opening brings the same window and the same project back.
    dialog.hide();
    dialog.show();
    QCoreApplication::processEvents();

    CHECK(dialog.class_list()->count() == 2);
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
    CHECK(dialog.class_list()->count() == 3);
    CHECK(dialog.class_list()->item(2)->text() == QStringLiteral("Enemy"));
    CHECK(dialog.structure()->selected_class() == 2);
    CHECK_FALSE(dialog.status_label()->text().isEmpty());

    // A duplicate name, an empty name and a malformed expression are refused.
    CHECK_FALSE(dialog.create_class(QStringLiteral("Enemy"), QStringLiteral("app+0x40")));
    CHECK(fixture.project.classes().size() == 3);
    CHECK_FALSE(dialog.create_class(QStringLiteral("  "), QStringLiteral("app+0x40")));
    CHECK_FALSE(dialog.create_class(QStringLiteral("Other"), QStringLiteral("app+")));
    CHECK_FALSE(dialog.create_class(QStringLiteral("Other"), QString()));
    CHECK(fixture.project.classes().size() == 3);

    CHECK(dialog.rename_selected_class(QStringLiteral("Boss")));
    CHECK(dialog.class_list()->item(2)->text() == QStringLiteral("Boss"));
    CHECK_FALSE(dialog.rename_selected_class(QStringLiteral("Player")));
    CHECK(dialog.class_list()->item(2)->text() == QStringLiteral("Boss"));

    CHECK(dialog.set_selected_class_address(QStringLiteral("game.exe+0x99")));
    CHECK(fixture.project.classes()[2].address_expression == "game.exe+0x99");
    CHECK_FALSE(dialog.set_selected_class_address(QStringLiteral("app+")));
    CHECK(fixture.project.classes()[2].address_expression == "game.exe+0x99");

    dialog.delete_class();
    CHECK(fixture.project.classes().size() == 2);
    CHECK(dialog.class_list()->count() == 2);

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
    dialog.insert_node(NodeKind::int32);
    REQUIRE(player.nodes.size() == 3);
    CHECK(player.nodes[2].kind == NodeKind::int32);
    CHECK(player.nodes[2].name == "field_9");

    // Inserting with a row selected puts the node before it.
    dialog.grid()->setCurrentIndex(dialog.structure()->index(1, 0));
    dialog.insert_node(NodeKind::float32);
    REQUIRE(player.nodes.size() == 4);
    CHECK(player.nodes[1].kind == NodeKind::float32);
    CHECK(player.nodes[1].name == "field_1");

    // Shrinking a node's type pads the freed bytes so offsets stay put.
    dialog.grid()->setCurrentIndex(dialog.structure()->index(1, 0));
    dialog.change_node_kind(NodeKind::int8);
    REQUIRE(player.nodes.size() == 6);
    CHECK(player.nodes[1].kind == NodeKind::int8);
    CHECK(player.nodes[2].kind == NodeKind::hex16);
    CHECK(player.nodes[3].kind == NodeKind::hex8);
    CHECK(player.nodes[3].name.empty());

    // Show as hex acts on the selection, and Delete removes it.
    dialog.grid()->setCurrentIndex(dialog.structure()->index(1, 0));
    dialog.set_node_hex(true);
    CHECK(player.nodes[1].hex);
    dialog.set_node_hex(false);
    CHECK_FALSE(player.nodes[1].hex);

    dialog.copy_name();
    CHECK(dialog.status_label()->text() == QStringLiteral("Name copied."));

    dialog.grid()->setCurrentIndex(dialog.structure()->index(1, 0));
    dialog.remove_selected_nodes();
    CHECK(player.nodes.size() == 5);
    CHECK(player.nodes[1].kind == NodeKind::hex16);

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

    REQUIRE(dialog.class_list()->count() == 2);

    fixture.project.clear();
    fixture.project.add_class("Only", "app+0x10");
    dialog.refresh_project();

    REQUIRE(dialog.class_list()->count() == 1);
    CHECK(dialog.class_list()->item(0)->text() == QStringLiteral("Only"));
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
    dialog.set_address_mode(slopkit::ui::AddressMode::absolute);
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

    REQUIRE(point.nodes[2].kind == NodeKind::hex64);
    dialog.dissect();
    drain_until_idle(fixture.worker);

    CHECK(point.nodes[2].kind == NodeKind::pointer);
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
    REQUIRE(player.nodes[1].kind == NodeKind::pointer);

    dialog.grid()->setCurrentIndex(dialog.structure()->index(1, 0));
    dialog.set_followed_class(QStringLiteral("Point"));

    CHECK(player.nodes[1].kind == NodeKind::class_pointer);
    CHECK(player.nodes[1].class_name == "Point");
    CHECK(dialog.structure()->is_expanded(player.nodes[1].id));
    CHECK(dialog.structure()->rowCount(dialog.structure()->index(1, 0)) == 2);
    CHECK(dialog.status_label()->text() == QStringLiteral("Following 'Point'."));

    dialog.grid()->setCurrentIndex(dialog.structure()->index(1, 0));
    dialog.set_followed_class(QString());
    CHECK(player.nodes[1].kind == NodeKind::pointer);
    CHECK(player.nodes[1].class_name.empty());
    CHECK(dialog.structure()->rowCount(dialog.structure()->index(1, 0)) == 0);

    dialog.set_followed_class(QStringLiteral("Nobody"));
    CHECK(player.nodes[1].kind == NodeKind::pointer);
    CHECK(dialog.status_label()->text() == QStringLiteral("Unknown class 'Nobody'."));

    // A node that is not a pointer cannot be followed.
    dialog.grid()->setCurrentIndex(dialog.structure()->index(0, 0));
    dialog.set_followed_class(QStringLiteral("Point"));
    CHECK(player.nodes[0].kind == NodeKind::int8);
    CHECK(dialog.status_label()->text() == QStringLiteral("Select a pointer node first."));

    dialog.close();
}
