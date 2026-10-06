#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

#include "debug/access_watch.hpp"
#include "ui/models/watch_hits_model.hpp"

namespace
{
    // Proves the model renders through the formatter it was handed.
    QString format(std::uint64_t address)
    {
        return QStringLiteral("#") + QString::number(address, 16);
    }

    slopkit::debug::AccessWatch watching(std::uint64_t address)
    {
        slopkit::debug::AccessWatch watch;
        watch.start(address, slopkit::debug::Kind::hardware_write, 4, 1);
        return watch;
    }
} // namespace

TEST_CASE("watch hits coalesce one row per instruction", "[ui][models]")
{
    application();

    slopkit::debug::AccessWatch watch = watching(0xABC);
    watch.record(11, 0x1010);
    watch.record(11, 0x1010);
    watch.record(12, 0x2020);

    slopkit::ui::models::WatchHitsModel model(nullptr, format);
    model.sync(watch);

    REQUIRE(model.rowCount() == 2);
    CHECK(model.data(model.index(0, slopkit::ui::models::WatchHitsModel::instruction), Qt::DisplayRole).toString()
          == QStringLiteral("#1010"));
    CHECK(model.count_at(0) == 2);
    CHECK(model.instruction_at(0) == 0x1010);
    CHECK(model.data(model.index(0, slopkit::ui::models::WatchHitsModel::thread), Qt::DisplayRole).toString()
          == QStringLiteral("11"));
    CHECK(model.count_at(1) == 1);
    CHECK(model.data(model.index(1, slopkit::ui::models::WatchHitsModel::thread), Qt::DisplayRole).toString()
          == QStringLiteral("12"));

    // A further hit on the first instruction bumps its count in place instead of
    // adding a row.
    watch.record(11, 0x1010);
    model.sync(watch);
    REQUIRE(model.rowCount() == 2);
    CHECK(model.count_at(0) == 3);
    CHECK(model.instruction_at(1) == 0x2020);

    // A new watch replaces every row.
    watch.start(0xDEF, slopkit::debug::Kind::hardware_read_write, 4, 2);
    watch.record(1, 0x3030);
    model.sync(watch);
    REQUIRE(model.rowCount() == 1);
    CHECK(model.instruction_at(0) == 0x3030);
}

TEST_CASE("watch hits fill in the decoded text on demand", "[ui][models]")
{
    application();

    slopkit::debug::AccessWatch watch = watching(0xABC);
    watch.record(5, 0x1010);

    slopkit::ui::models::WatchHitsModel model(nullptr, format);
    model.sync(watch);

    REQUIRE(model.rowCount() == 1);
    // Before the decoder answers, the row shows a placeholder and no tooltip.
    CHECK(model.text_at(0) == QStringLiteral("??"));
    CHECK(model.data(model.index(0, slopkit::ui::models::WatchHitsModel::text), Qt::DisplayRole).toString()
          == QStringLiteral("??"));
    CHECK(model.data(model.index(0, slopkit::ui::models::WatchHitsModel::instruction), Qt::ToolTipRole)
              .toString()
              .isEmpty());
    CHECK(model.needs_text(0));

    // The recovered address becomes the tooltip; the text fills the row.
    model.set_text(0, 0x1008, QStringLiteral("mov eax, [rbx]"));
    CHECK_FALSE(model.needs_text(0));
    CHECK(model.text_at(0) == QStringLiteral("mov eax, [rbx]"));
    CHECK(model.display_instruction_at(0) == 0x1008);
    CHECK(model.data(model.index(0, slopkit::ui::models::WatchHitsModel::instruction), Qt::ToolTipRole).toString()
          == QStringLiteral("#1008"));

    // Out-of-range rows stay quiet.
    CHECK(model.text_at(9).isEmpty());
    CHECK(model.instruction_at(9) == 0);
    CHECK(model.count_at(9) == 0);
}
