#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "structs/export.hpp"
#include "structs/types.hpp"
#include "support/struct_document_helpers.hpp"
#include "support/ui_helpers.hpp"
#include "ui/dialogs/struct_viewer.hpp"
#include "ui/models/struct_model.hpp"
#include "ui/text.hpp"

#include <QClipboard>
#include <QGuiApplication>
#include <QPlainTextEdit>

namespace
{
    using slopkit::structs::FieldKind;
    using slopkit::structs::FieldType;

    FieldType integer(std::size_t width)
    {
        FieldType type;
        type.kind  = FieldKind::integer;
        type.width = width;
        return type;
    }
}

TEST_CASE("the struct viewer lists the definitions and reads live values", "[ui]")
{
    application();
    StructFixture                            fixture;
    slopkit::ui::dialogs::StructViewerDialog viewer {fixture.worker, fixture.target, fixture.definitions};

    CHECK(viewer.model()->rowCount() == 0);

    const std::uint64_t root = fixture.definitions.add_struct("Player", "0x1800");
    fixture.definitions.add_field(root, "health", 0x10, integer(4));
    viewer.refresh_definitions();
    CHECK(viewer.model()->rowCount() == 2);

    put_u32(*fixture.access.memory, 0x1810, 55);
    fixture.drain();
    const auto                            requests = viewer.next_live_request();
    std::vector<slopkit::ui::LiveReading> readings;
    readings.reserve(requests.size());
    for (const slopkit::ui::LiveRequest& request : requests)
    {
        readings.push_back(reading(request.id, *fixture.access.memory, request.address, request.size));
    }
    viewer.apply_live_readings(readings);

    CHECK(viewer.model()
              ->data(viewer.model()->index(1, slopkit::ui::models::StructModel::value), Qt::DisplayRole)
              .toString()
          == QStringLiteral("55"));
}

TEST_CASE("the struct viewer toggles hex and removes a selected node", "[ui]")
{
    application();
    StructFixture                            fixture;
    slopkit::ui::dialogs::StructViewerDialog viewer {fixture.worker, fixture.target, fixture.definitions};

    const std::uint64_t root   = fixture.definitions.add_struct("Player", "0x1800");
    const std::uint64_t health = fixture.definitions.add_field(root, "health", 0x10, integer(4));
    viewer.refresh_definitions();
    REQUIRE(viewer.model()->rowCount() == 2);

    viewer.table_view()->setCurrentIndex(viewer.model()->index(1, slopkit::ui::models::StructModel::name));
    viewer.hex_action()->trigger();
    CHECK(fixture.definitions.find(health)->hex);

    viewer.table_view()->setCurrentIndex(viewer.model()->index(1, slopkit::ui::models::StructModel::name));
    viewer.remove_action()->trigger();
    CHECK(fixture.definitions.size() == 1);
    CHECK(fixture.definitions.find(health) == nullptr);
    CHECK(viewer.model()->rowCount() == 1);
}

TEST_CASE("the struct viewer follows and unfollows a pointer", "[ui]")
{
    application();
    StructFixture                            fixture;
    slopkit::ui::dialogs::StructViewerDialog viewer {fixture.worker, fixture.target, fixture.definitions};

    const std::uint64_t root = fixture.definitions.add_struct("Player", "0x1800");
    const std::uint64_t next = fixture.definitions.add_field(root, "next", 0x20, FieldType {FieldKind::pointer});
    fixture.definitions.add_field(next, "value", 0x8, integer(4));
    viewer.refresh_definitions();

    put_u64(*fixture.access.memory, 0x1820, 0x3000);
    viewer.table_view()->setCurrentIndex(viewer.model()->index(1, slopkit::ui::models::StructModel::name));
    CHECK(viewer.follow_action()->isEnabled());
    CHECK_FALSE(viewer.unfollow_action()->isEnabled());

    viewer.follow_action()->trigger();
    CHECK_FALSE(viewer.follow_action()->isEnabled());
    CHECK(viewer.unfollow_action()->isEnabled());

    viewer.unfollow_action()->trigger();
    CHECK(viewer.follow_action()->isEnabled());
    CHECK_FALSE(viewer.unfollow_action()->isEnabled());
}

TEST_CASE("the struct viewer previews and copies the C++ declaration", "[ui]")
{
    application();
    StructFixture                            fixture;
    slopkit::ui::dialogs::StructViewerDialog viewer {fixture.worker, fixture.target, fixture.definitions};

    const std::uint64_t root = fixture.definitions.add_struct("Player", "0x1800");
    fixture.definitions.add_field(root, "health", 0x10, integer(4));
    viewer.refresh_definitions();

    const QString expected = slopkit::ui::to_qstring(slopkit::structs::render_cpp(fixture.definitions, 8));
    CHECK(viewer.preview()->toPlainText() == expected);
    CHECK(viewer.preview()->isReadOnly());
    CHECK(viewer.export_action()->isEnabled());

    viewer.copy_action()->trigger();
    CHECK(QGuiApplication::clipboard()->text() == expected);
}

TEST_CASE("the struct viewer carries the documented identity", "[ui]")
{
    application();
    StructFixture                            fixture;
    slopkit::ui::dialogs::StructViewerDialog viewer {fixture.worker, fixture.target, fixture.definitions};

    CHECK(viewer.objectName() == QStringLiteral("struct_viewer"));
    CHECK(viewer.windowTitle() == QStringLiteral("Struct View"));
    CHECK_FALSE(viewer.testAttribute(Qt::WA_QuitOnClose));
    CHECK(viewer.table_view()->objectName() == QStringLiteral("struct_table"));
    CHECK(viewer.status_label() != nullptr);
    CHECK(viewer.add_struct_action() != nullptr);
    CHECK(viewer.add_field_action() != nullptr);
    CHECK(viewer.follow_action() != nullptr);
    CHECK(viewer.unfollow_action() != nullptr);
    CHECK(viewer.copy_action() != nullptr);
    CHECK(viewer.export_action() != nullptr);
    CHECK(viewer.preview()->isReadOnly());
}
