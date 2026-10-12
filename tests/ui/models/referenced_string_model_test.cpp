#include <catch2/catch.hpp>

#include <cstdint>
#include <utility>

#include "support/ui_helpers.hpp"
#include "ui/components/elided_tooltip_delegate.hpp"
#include "ui/models/referenced_string_model.hpp"

namespace
{
    QString format(std::uint64_t address)
    {
        return QStringLiteral("@") + QString::number(address, 16);
    }

    slopkit::ui::models::ReferencedStringRow
    row(QString text, std::uint64_t string_address, QString module, std::uint64_t instruction)
    {
        return slopkit::ui::models::ReferencedStringRow {.text           = std::move(text),
                                                         .string_address = string_address,
                                                         .module         = std::move(module),
                                                         .instruction    = instruction};
    }
}

TEST_CASE("the referenced-string model exposes the four columns", "[ui]")
{
    application();

    slopkit::ui::models::ReferencedStringModel model(nullptr, format);
    model.set_rows({row(QStringLiteral("hello"), 0x3010, QStringLiteral("app"), 0x1000)});

    using Model = slopkit::ui::models::ReferencedStringModel;
    CHECK(model.columnCount() == Model::column_count);
    CHECK(model.rowCount() == 1);
    CHECK(model.headerData(Model::string, Qt::Horizontal, Qt::DisplayRole).toString() == QStringLiteral("String"));
    CHECK(model.headerData(Model::address, Qt::Horizontal, Qt::DisplayRole).toString() == QStringLiteral("Address"));
    CHECK(model.headerData(Model::module, Qt::Horizontal, Qt::DisplayRole).toString() == QStringLiteral("Module"));
    CHECK(model.headerData(Model::referenced_by, Qt::Horizontal, Qt::DisplayRole).toString()
          == QStringLiteral("Referenced by"));

    CHECK(model.data(model.index(0, Model::string), Qt::DisplayRole).toString() == QStringLiteral("hello"));
    CHECK(model.data(model.index(0, Model::address), Qt::DisplayRole).toString() == QStringLiteral("@3010"));
    CHECK(model.data(model.index(0, Model::module), Qt::DisplayRole).toString() == QStringLiteral("app"));
    CHECK(model.data(model.index(0, Model::referenced_by), Qt::DisplayRole).toString() == QStringLiteral("@1000"));

    CHECK(model.data(model.index(0, Model::string), Qt::ToolTipRole).toString() == QStringLiteral("hello"));
    CHECK(model.data(model.index(0, Model::string), slopkit::ui::widgets::kFullTextRole).toString()
          == QStringLiteral("hello"));

    CHECK(model.instruction_at(0) == 0x1000);
    CHECK(model.instruction_at(5) == 0);
}
