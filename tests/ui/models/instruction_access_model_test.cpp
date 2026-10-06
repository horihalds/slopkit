#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

#include "ui/fonts.hpp"
#include "ui/models/instruction_access_model.hpp"

namespace
{
    // Proves the model renders through the formatter it was handed.
    QString format(std::uint64_t address)
    {
        return QStringLiteral("@") + QString::number(address, 16);
    }

    slopkit::ui::ResolvedAccess access(std::uint64_t address, std::size_t width, bool writes, bool resolved = true)
    {
        return slopkit::ui::ResolvedAccess {address, QStringLiteral("[RBX+10]"), width, writes, resolved};
    }
} // namespace

TEST_CASE("instruction accesses render through the injected formatter", "[ui][models]")
{
    application();

    slopkit::ui::models::InstructionAccessModel model(nullptr, format);
    model.set({access(0x1040, 4, false), access(0, 8, true, false)});

    REQUIRE(model.row_count() == 2);
    CHECK(model.data(model.index(0, slopkit::ui::models::InstructionAccessModel::address), Qt::DisplayRole).toString()
          == QStringLiteral("@1040"));
    CHECK(model.data(model.index(0, slopkit::ui::models::InstructionAccessModel::width), Qt::DisplayRole).toString()
          == QStringLiteral("4"));
    CHECK(model.data(model.index(0, slopkit::ui::models::InstructionAccessModel::access), Qt::DisplayRole).toString()
          == QStringLiteral("read"));

    // An operand the register context could not resolve shows "?" and explains
    // itself, while its width still counts.
    CHECK(model.data(model.index(1, slopkit::ui::models::InstructionAccessModel::address), Qt::DisplayRole).toString()
          == QStringLiteral("?"));
    CHECK(model.data(model.index(1, slopkit::ui::models::InstructionAccessModel::access), Qt::DisplayRole).toString()
          == QStringLiteral("write"));
    CHECK_FALSE(model.resolved_at(1));
    CHECK(model.resolved_at(0));
    CHECK(model.any_resolved());
    CHECK(model.address_at(0) == 0x1040);
    CHECK(model.width_at(1) == 8);
    CHECK_FALSE(model.data(model.index(1, slopkit::ui::models::InstructionAccessModel::address), Qt::ToolTipRole)
                    .toString()
                    .isEmpty());
}

TEST_CASE("instruction accesses render in the embedded mono font", "[ui][models]")
{
    application();

    slopkit::ui::models::InstructionAccessModel model(nullptr, format);
    model.set({access(0x1040, 4, false)});
    REQUIRE(model.row_count() == 1);

    const QString family = slopkit::ui::mono_font().family();
    using slopkit::ui::models::InstructionAccessModel;
    CHECK(model.data(model.index(0, InstructionAccessModel::address), Qt::FontRole).value<QFont>().family() == family);
    CHECK(model.data(model.index(0, InstructionAccessModel::operand), Qt::FontRole).value<QFont>().family() == family);
    CHECK(model.data(model.index(0, InstructionAccessModel::width), Qt::FontRole).value<QFont>().family() == family);

    // The access word column keeps the UI font.
    CHECK_FALSE(model.data(model.index(0, InstructionAccessModel::access), Qt::FontRole).isValid());
}

TEST_CASE("instruction accesses clear to an empty table", "[ui][models]")
{
    application();

    slopkit::ui::models::InstructionAccessModel model(nullptr, format);
    model.set({});
    CHECK(model.row_count() == 0);
    CHECK_FALSE(model.any_resolved());
    CHECK_FALSE(model.resolved_at(0));
    CHECK(model.address_at(0) == 0);
    CHECK(model.width_at(0) == 0);
}
