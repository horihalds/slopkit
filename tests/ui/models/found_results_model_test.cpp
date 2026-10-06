#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

TEST_CASE("the found-results model mirrors a snapshot", "[ui]")
{
    application();

    slopkit::ui::models::FoundResultsModel model;

    slopkit::scan::ScanConfig config;
    config.value_type = slopkit::scan::ValueType::int32;

    slopkit::scan::ScanSnapshot snapshot;
    snapshot.hit_count = 3; // Two of the three matches are on the display page.
    snapshot.hits.push_back(slopkit::scan::ScanHit {
        0x2000, std::vector<std::byte> {std::byte {2}, std::byte {0}, std::byte {0}, std::byte {0}},
          {}
    });
    snapshot.hits.push_back(slopkit::scan::ScanHit {
        0x1000, std::vector<std::byte> {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}},
          {}
    });

    model.set_snapshot(snapshot, config);

    CHECK(model.rowCount() == 2);
    CHECK(model.columnCount() == 3);
    CHECK(model.headerData(slopkit::ui::models::FoundResultsModel::address, Qt::Horizontal, Qt::DisplayRole).toString()
          == QStringLiteral("Address"));

    // Default order is by address, ascending.
    CHECK(model.hit_at(0)->address == 0x1000);
    CHECK(model.data(model.index(0, slopkit::ui::models::FoundResultsModel::address), Qt::DisplayRole).toString()
          == QStringLiteral("1000"));
    CHECK(model.data(model.index(1, slopkit::ui::models::FoundResultsModel::value), Qt::DisplayRole).toString()
          == QStringLiteral("2"));
    CHECK(model.data(model.index(0, slopkit::ui::models::FoundResultsModel::previous), Qt::DisplayRole)
              .toString()
              .isEmpty());

    model.sort(slopkit::ui::models::FoundResultsModel::address, Qt::DescendingOrder);
    CHECK(model.hit_at(0)->address == 0x2000);

    model.clear();
    CHECK(model.rowCount() == 0);
    CHECK(model.hit_at(0) == nullptr);
}

TEST_CASE("the found-results model renders the clipboard texts", "[ui]")
{
    application();

    slopkit::ui::models::FoundResultsModel model;

    slopkit::scan::ScanConfig config;
    config.value_type = slopkit::scan::ValueType::int32;

    slopkit::scan::ScanSnapshot snapshot;
    snapshot.hit_count = 2;
    snapshot.hits.push_back(slopkit::scan::ScanHit {
        0x1040, std::vector<std::byte> {std::byte {10}, std::byte {0}, std::byte {0}, std::byte {0}},
          {}
    });
    snapshot.hits.push_back(slopkit::scan::ScanHit {
        0x7F3A1B2C, std::vector<std::byte> {std::byte {100}, std::byte {0}, std::byte {0}, std::byte {0}},
          {}
    });
    model.set_snapshot(snapshot, config);

    // With no module map every hit copies as an absolute address.
    CHECK(model.copy_text(0, slopkit::ui::models::CopyFormat::module_relative) == QStringLiteral("1040"));

    model.set_modules({module_image("app", 0x1000, 0x1000)});

    // A static hit inside `app`.
    CHECK(model.copy_text(0, slopkit::ui::models::CopyFormat::module_relative) == QStringLiteral("app+40"));
    CHECK(model.copy_text(0, slopkit::ui::models::CopyFormat::absolute) == QStringLiteral("1040"));
    CHECK(model.copy_text(0, slopkit::ui::models::CopyFormat::address_and_value) == QStringLiteral("app+40: 10"));

    // A dynamic hit falls back to the absolute form for module + RVA.
    CHECK(model.copy_text(1, slopkit::ui::models::CopyFormat::module_relative) == QStringLiteral("7F3A1B2C"));
    CHECK(model.copy_text(1, slopkit::ui::models::CopyFormat::absolute) == QStringLiteral("7F3A1B2C"));
    CHECK(model.copy_text(1, slopkit::ui::models::CopyFormat::address_and_value) == QStringLiteral("7F3A1B2C: 100"));

    // Module + RVA ignores the display mode; address + value follows it.
    model.set_address_mode(slopkit::ui::AddressMode::absolute);
    CHECK(model.copy_text(0, slopkit::ui::models::CopyFormat::module_relative) == QStringLiteral("app+40"));
    CHECK(model.copy_text(0, slopkit::ui::models::CopyFormat::address_and_value) == QStringLiteral("1040: 10"));

    // An out-of-range row and a cleared model yield nothing.
    CHECK(model.copy_text(2, slopkit::ui::models::CopyFormat::absolute).isEmpty());
    CHECK(model.copy_text(-1, slopkit::ui::models::CopyFormat::module_relative).isEmpty());
    model.clear();
    CHECK(model.copy_text(0, slopkit::ui::models::CopyFormat::absolute).isEmpty());
}

TEST_CASE("the found-results model copies the value in hex when configured", "[ui]")
{
    application();

    slopkit::ui::models::FoundResultsModel model;

    slopkit::scan::ScanConfig config;
    config.value_type = slopkit::scan::ValueType::int32;
    config.hex        = true;

    slopkit::scan::ScanSnapshot snapshot;
    snapshot.hit_count = 1;
    snapshot.hits.push_back(slopkit::scan::ScanHit {
        0x1040, std::vector<std::byte> {std::byte {10}, std::byte {0}, std::byte {0}, std::byte {0}},
          {}
    });
    model.set_snapshot(snapshot, config);
    model.set_modules({module_image("app", 0x1000, 0x1000)});

    // The value matches the Value column's hex rendering, zero-padded to int32.
    CHECK(model.copy_text(0, slopkit::ui::models::CopyFormat::address_and_value) == QStringLiteral("app+40: 0000000A"));
}

TEST_CASE("the found-results model marks and groups static hits", "[ui]")
{
    application();

    slopkit::ui::models::FoundResultsModel model;

    slopkit::scan::ScanConfig config;
    config.value_type = slopkit::scan::ValueType::int32;

    slopkit::scan::ScanSnapshot snapshot;
    snapshot.hit_count = 3;
    snapshot.hits.push_back(slopkit::scan::ScanHit {0x1000, std::vector<std::byte> {std::byte {1}}, {}});
    snapshot.hits.push_back(slopkit::scan::ScanHit {0x5000000, std::vector<std::byte> {std::byte {5}}, {}});
    snapshot.hits.push_back(slopkit::scan::ScanHit {0x2000, std::vector<std::byte> {std::byte {2}}, {}});
    model.set_snapshot(snapshot, config);

    // No map yet: the plain ascending address order and no static hit.
    CHECK(model.hit_at(0)->address == 0x1000);
    CHECK(model.hit_at(1)->address == 0x2000);
    CHECK(model.hit_at(2)->address == 0x5000000);
    CHECK_FALSE(model.is_static(0x1000));

    // Images deliberately out of order; the model sorts them.
    std::vector<slopkit::process::ModuleInfo> modules;
    modules.push_back(module_image("low", 0x2000, 0x1000));
    modules.push_back(module_image("first", 0x1000, 0x800));
    model.set_modules(modules);

    // Static hits group first in address order, the heap hit last.
    CHECK(model.hit_at(0)->address == 0x1000);
    CHECK(model.hit_at(1)->address == 0x2000);
    CHECK(model.hit_at(2)->address == 0x5000000);
    CHECK(model.is_static(0x1000));
    CHECK(model.is_static(0x17FF));
    CHECK_FALSE(model.is_static(0x1800)); // Half-open: base + size is outside.
    CHECK_FALSE(model.is_static(0x5000000));

    // Static hits render as module+RVA by default; the heap hit stays absolute.
    CHECK(model.data(model.index(0, slopkit::ui::models::FoundResultsModel::address), Qt::DisplayRole).toString()
          == QStringLiteral("first+0"));
    CHECK(model.data(model.index(1, slopkit::ui::models::FoundResultsModel::address), Qt::DisplayRole).toString()
          == QStringLiteral("low+0"));
    CHECK(model.data(model.index(2, slopkit::ui::models::FoundResultsModel::address), Qt::DisplayRole).toString()
          == QStringLiteral("5000000"));

    // Switching to absolute restores the raw address text everywhere.
    model.set_address_mode(slopkit::ui::AddressMode::absolute);
    CHECK(model.data(model.index(0, slopkit::ui::models::FoundResultsModel::address), Qt::DisplayRole).toString()
          == QStringLiteral("1000"));
    CHECK(model.data(model.index(2, slopkit::ui::models::FoundResultsModel::address), Qt::DisplayRole).toString()
          == QStringLiteral("5000000"));
    model.set_address_mode(slopkit::ui::AddressMode::module_relative);

    const QColor green = slopkit::ui::active_theme().success;
    CHECK(model.data(model.index(0, slopkit::ui::models::FoundResultsModel::address), Qt::ForegroundRole)
              .value<QBrush>()
              .color()
          == green);
    CHECK(model.data(model.index(1, slopkit::ui::models::FoundResultsModel::address), Qt::ForegroundRole)
              .value<QBrush>()
              .color()
          == green);
    CHECK_FALSE(
        model.data(model.index(2, slopkit::ui::models::FoundResultsModel::address), Qt::ForegroundRole).isValid());

    // The tier is the primary key: the main-image hit (the lowest-based image
    // when nothing is flagged) stays above the other static hit, which stays
    // above the dynamic one, whatever the sort order.
    model.sort(slopkit::ui::models::FoundResultsModel::address, Qt::DescendingOrder);
    CHECK(model.hit_at(0)->address == 0x1000);
    CHECK(model.hit_at(1)->address == 0x2000);
    CHECK(model.hit_at(2)->address == 0x5000000);

    // Dropping the map makes every hit non-static again.
    model.set_modules({});
    CHECK_FALSE(model.is_static(0x1000));
    CHECK_FALSE(model.is_static(0x2000));
    CHECK(model.rowCount() == 3);

    // A flagged main image outranks the other static image, which outranks the
    // dynamic hit, even though the flagged image has the higher base.
    model.sort(slopkit::ui::models::FoundResultsModel::address, Qt::AscendingOrder);
    slopkit::process::ModuleInfo game = module_image("game", 0x2000, 0x1000);
    game.is_main                      = true;
    model.set_modules({game, module_image("lib", 0x1000, 0x800)});

    CHECK(model.is_main_hit(0x2000));       // base included
    CHECK(model.is_main_hit(0x2FFF));       // base + size - 1 included
    CHECK_FALSE(model.is_main_hit(0x3000)); // half-open end excluded
    CHECK_FALSE(model.is_main_hit(0x1000));
    CHECK(model.hit_at(0)->address == 0x2000);    // main image
    CHECK(model.hit_at(1)->address == 0x1000);    // other static image
    CHECK(model.hit_at(2)->address == 0x5000000); // dynamic
}

TEST_CASE("the found-results model orders the whole result set", "[ui]")
{
    application();

    slopkit::ui::models::FoundResultsModel model;

    slopkit::scan::ScanConfig config;
    config.value_type = slopkit::scan::ValueType::int32;

    const auto whole = late_static_result();
    model.set_snapshot(snapshot_over(whole), config);
    model.set_modules({module_image("late", 0x500000, 0x100)});

    SECTION("the page shows the whole-list order")
    {
        // The page alone holds no static hit, yet the whole-list ordering puts the
        // late static hit first and still shows one page of rows.
        CHECK(model.rowCount() == slopkit::scan::kDisplayPage);
        CHECK(model.hit_at(0)->address == 0x500000);
        CHECK(model.is_static(model.hit_at(0)->address));
        CHECK_FALSE(model.is_static(model.hit_at(1)->address));
        // The rows after the static hit are the smallest addresses of the whole set.
        CHECK(model.hit_at(1)->address == 0x100000);
        CHECK(model.hit_at(255)->address == 0x100000 + 254 * 4);
    }

    SECTION("sorting is over the whole result set")
    {
        model.sort(slopkit::ui::models::FoundResultsModel::value, Qt::DescendingOrder);

        // Statics stay grouped first; the largest value of the whole set, which is
        // beyond the page, is shown right after it.
        CHECK(model.rowCount() == slopkit::scan::kDisplayPage);
        CHECK(model.hit_at(0)->address == 0x500000);
        CHECK(model.hit_at(0)->value == whole->at(280).value);
        CHECK(model.hit_at(1)->address == 0x100000 + 299 * 4);
        CHECK(model.hit_at(2)->address == 0x100000 + 298 * 4);
    }
}
