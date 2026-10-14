#include <catch2/catch.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QObject>
#include <QTreeWidgetItem>

#include "reclass/node.hpp"
#include "reclass/nodes/container.hpp"
#include "reclass/project.hpp"
#include "support/reclass_helpers.hpp"
#include "support/ui_helpers.hpp"
#include "ui/components/project_view.hpp"

namespace
{
    using slopkit::reclass::Class;
    using slopkit::reclass::NodeKind;
    using slopkit::reclass::Project;
    using slopkit::test::node_of;
    using slopkit::test::wrapper_array_of;
    using slopkit::ui::components::ProjectView;
}

TEST_CASE("the project tree lists the classes and the enums", "[reclass][ui]")
{
    application();

    Project project;
    project.add_class("Point", "app+0x20");
    project.add_class("Player", "app+0x10");
    project.add_enum("State", 4);

    ProjectView view;
    view.set_project(project);

    REQUIRE(view.class_count() == 2);
    CHECK(view.class_name(0) == QStringLiteral("Point"));
    CHECK(view.class_name(1) == QStringLiteral("Player"));

    REQUIRE(view.topLevelItemCount() == 2);
    CHECK(view.topLevelItem(0)->text(0) == QStringLiteral("Classes"));
    CHECK(view.topLevelItem(1)->text(0) == QStringLiteral("Enums"));
    REQUIRE(view.topLevelItem(0)->childCount() == 2);
    REQUIRE(view.topLevelItem(1)->childCount() == 1);
    CHECK(view.topLevelItem(1)->child(0)->text(0) == QStringLiteral("State"));

    std::vector<std::size_t> selected;
    QObject::connect(&view,
                     &ProjectView::classSelected,
                     &view,
                     [&selected](std::size_t index)
                     {
                         selected.push_back(index);
                     });

    view.select_class(1);
    REQUIRE(selected.size() == 1);
    CHECK(selected[0] == 1);
    CHECK(view.selected_class() == std::optional<std::size_t> {1});

    // A rebuild keeps the selected class instead of snapping back to the first.
    view.refresh();
    CHECK(view.selected_class() == std::optional<std::size_t> {1});
    CHECK(selected.size() == 1);
}

TEST_CASE("the class hierarchy view nests the classes a class refers to", "[reclass][ui]")
{
    application();

    Project project;
    project.add_class("Point", "app+0x20");
    project.add_class("Player", "app+0x10");

    auto next  = wrapper_array_of(project, "Point", 2);
    next->name = "path";
    project.classes()[1].nodes.push_back(std::move(next));

    // Point points back at Player, so the hierarchy is cyclic.
    auto back        = node_of(project, NodeKind::class_pointer, "owner");
    back->class_name = "Player";
    project.classes()[0].nodes.push_back(std::move(back));

    ProjectView view;
    view.set_project(project);

    // Flat by default: every class is a leaf.
    REQUIRE(view.topLevelItem(0)->childCount() == 2);
    CHECK(view.topLevelItem(0)->child(1)->childCount() == 0);

    view.set_hierarchy_view(true);
    CHECK(view.hierarchy_view());

    // Player now nests Point, and Point's nested item stops at the cycle
    // instead of recursing forever.
    QTreeWidgetItem* classes = view.topLevelItem(0);
    REQUIRE(classes->child(1)->text(0) == QStringLiteral("Player"));
    REQUIRE(classes->child(1)->childCount() == 1);
    CHECK(classes->child(1)->child(0)->text(0) == QStringLiteral("Point"));
    CHECK(classes->child(1)->child(0)->childCount() == 0);

    view.set_hierarchy_view(false);
    CHECK(view.topLevelItem(0)->child(1)->childCount() == 0);
}

TEST_CASE("a class tooltip is set by index", "[reclass][ui]")
{
    application();

    Project project;
    project.add_class("Point", "app+0x20");

    ProjectView view;
    view.set_project(project);

    view.set_class_tooltip(0, QStringLiteral("app+0x20 → 0x40"));
    REQUIRE(view.class_item(0) != nullptr);
    CHECK(view.class_item(0)->toolTip(0) == QStringLiteral("app+0x20 → 0x40"));
    CHECK(view.enum_item(0) == nullptr);
}
