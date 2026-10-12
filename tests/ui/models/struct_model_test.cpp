#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>

#include "structs/types.hpp"
#include "support/struct_document_helpers.hpp"
#include "support/ui_helpers.hpp"
#include "ui/components/indent_delegate.hpp"
#include "ui/models/struct_model.hpp"

namespace
{
    using slopkit::structs::FieldKind;
    using slopkit::structs::FieldType;
    using slopkit::ui::models::StructModel;

    FieldType integer(std::size_t width)
    {
        FieldType type;
        type.kind  = FieldKind::integer;
        type.width = width;
        return type;
    }
}

TEST_CASE("the struct model presents the definition columns", "[ui]")
{
    application();
    StructFixture fixture;

    const std::uint64_t root  = fixture.definitions.add_struct("Player", "0x1800");
    const std::uint64_t field = fixture.definitions.add_field(root, "health", 0x10, integer(4));

    StructModel model {fixture.definitions, fixture.document};
    CHECK(model.rowCount() == 2);
    CHECK(model.columnCount() == 4);
    CHECK(model.headerData(StructModel::offset, Qt::Horizontal, Qt::DisplayRole).toString()
          == QStringLiteral("Offset"));
    CHECK(model.headerData(StructModel::type, Qt::Horizontal, Qt::DisplayRole).toString() == QStringLiteral("Type"));
    CHECK(model.headerData(StructModel::name, Qt::Horizontal, Qt::DisplayRole).toString() == QStringLiteral("Name"));
    CHECK(model.headerData(StructModel::value, Qt::Horizontal, Qt::DisplayRole).toString() == QStringLiteral("Value"));

    CHECK(model.data(model.index(0, StructModel::name), Qt::DisplayRole).toString() == QStringLiteral("Player"));
    CHECK(model.data(model.index(1, StructModel::name), Qt::DisplayRole).toString() == QStringLiteral("health"));
    CHECK(model.data(model.index(1, StructModel::offset), Qt::DisplayRole).toString() == QStringLiteral("10"));
    CHECK(model.data(model.index(1, StructModel::type), Qt::DisplayRole).toString() == QStringLiteral("int32"));
    CHECK(model.data(model.index(1, StructModel::name), slopkit::ui::widgets::kDepthRole).toInt() == 1);
    CHECK(model.id_at(1) == field);

    CHECK(model.flags(model.index(1, StructModel::name)) & Qt::ItemIsEditable);
    CHECK(model.flags(model.index(1, StructModel::offset)) & Qt::ItemIsEditable);
    CHECK_FALSE(model.flags(model.index(0, StructModel::offset)) & Qt::ItemIsEditable);
    CHECK(model.flags(model.index(0, StructModel::name)) & Qt::ItemIsEditable);
}

TEST_CASE("editing a cell through the struct model updates the definition", "[ui]")
{
    application();
    StructFixture fixture;

    const std::uint64_t root  = fixture.definitions.add_struct("Player", "0x1800");
    const std::uint64_t field = fixture.definitions.add_field(root, "health", 0x10, integer(4));

    StructModel model {fixture.definitions, fixture.document};

    CHECK(model.setData(model.index(1, StructModel::name), QStringLiteral("hp"), Qt::EditRole));
    CHECK(fixture.definitions.find(field)->name == "hp");

    CHECK(model.setData(model.index(1, StructModel::offset), QStringLiteral("0x1C"), Qt::EditRole));
    CHECK(fixture.definitions.find(field)->offset == 0x1C);

    int statuses = 0;
    QObject::connect(&model,
                     &StructModel::statusChanged,
                     [&](const QString&, bool)
                     {
                         ++statuses;
                     });
    CHECK_FALSE(model.setData(model.index(1, StructModel::offset), QStringLiteral("not-a-number"), Qt::EditRole));
    CHECK(statuses == 1);

    CHECK_FALSE(model.setData(model.index(1, StructModel::name), QString(), Qt::EditRole));
}

TEST_CASE("the struct model pushes value edits to the document", "[ui]")
{
    application();
    StructFixture fixture;

    const std::uint64_t root  = fixture.definitions.add_struct("Player", "0x1800");
    const std::uint64_t field = fixture.definitions.add_field(root, "health", 0x10, integer(4));
    put_u32(*fixture.access.memory, 0x1810, 100);
    fixture.pass();

    StructModel model {fixture.definitions, fixture.document};
    CHECK(model.data(model.index(1, StructModel::value), Qt::DisplayRole).toString() == QStringLiteral("100"));
    CHECK_FALSE(model.flags(model.index(1, StructModel::type)) & Qt::ItemIsEditable);

    CHECK(model.setData(model.index(1, StructModel::value), QStringLiteral("7"), Qt::EditRole));
    CHECK(fixture.pump(
        [&]
        {
            return fixture.access.memory->at(0x1810) == std::byte {7};
        }));
    CHECK(fixture.document.value_text(field) == QStringLiteral("7"));
}
