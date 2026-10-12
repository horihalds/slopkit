#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "structs/definition.hpp"
#include "structs/layout.hpp"
#include "structs/types.hpp"

namespace
{
    using slopkit::structs::FieldKind;
    using slopkit::structs::FieldPlacement;
    using slopkit::structs::FieldType;
    using slopkit::structs::place;
    using slopkit::structs::plan_reads;
    using slopkit::structs::ReadPlan;
    using slopkit::structs::Structs;

    FieldType integer(std::size_t width)
    {
        FieldType type;
        type.kind  = FieldKind::integer;
        type.width = width;
        return type;
    }

    FieldPlacement placement_of(const std::vector<FieldPlacement>& placements, const Structs& structs, std::uint64_t id)
    {
        return placements[structs.index_of(id)];
    }
}

TEST_CASE("place resolves a root base and its field offsets", "[structs]")
{
    Structs             structs;
    const std::uint64_t player = structs.add_struct("Player", "1000");
    const std::uint64_t health = structs.add_field(player, "health", 0x10, integer(4));
    const std::uint64_t next   = structs.add_field(player, "next", 0x20, FieldType {FieldKind::pointer});

    const std::vector<std::string_view> names {"Player"};
    const std::vector<std::uint64_t>    bases {0x1000};
    const std::vector<std::uint64_t>    followed(structs.size(), 0);

    const auto placements = place(structs, names, bases, followed, 8);
    REQUIRE(placements.size() == structs.size());

    CHECK(placement_of(placements, structs, player).address == 0x1000);
    CHECK(placement_of(placements, structs, player).size == 0);
    CHECK(placement_of(placements, structs, health).address == 0x1010);
    CHECK(placement_of(placements, structs, health).size == 4);
    CHECK(placement_of(placements, structs, next).address == 0x1020);
    CHECK(placement_of(placements, structs, next).size == 8);

    const std::vector<std::uint8_t> visible(structs.size(), true);
    const ReadPlan                  plan = plan_reads(structs, placements, visible);
    CHECK(plan.ranges
          == std::vector<std::pair<std::uint64_t, std::size_t>> {
              {0x1010, 4},
              {0x1020, 8}
    });
    CHECK(plan.ids == std::vector<std::uint64_t> {health, next});
}

TEST_CASE("an embedded structure places its children inline", "[structs]")
{
    Structs             structs;
    const std::uint64_t player = structs.add_struct("Player", "1000");
    FieldType           embedded {FieldKind::structure};
    embedded.structure      = "Box";
    const std::uint64_t box = structs.add_field(player, "box", 0x10, embedded);
    const std::uint64_t x   = structs.add_field(box, "x", 0, integer(4));
    const std::uint64_t y   = structs.add_field(box, "y", 4, integer(4));

    const std::vector<std::string_view> names {"Player"};
    const std::vector<std::uint64_t>    bases {0x1000};
    const std::vector<std::uint64_t>    followed(structs.size(), 0);

    const auto placements = place(structs, names, bases, followed, 8);
    CHECK(placement_of(placements, structs, box).address == 0x1010);
    CHECK(placement_of(placements, structs, box).size == 0);
    CHECK(placement_of(placements, structs, x).address == 0x1010);
    CHECK(placement_of(placements, structs, y).address == 0x1014);
}

TEST_CASE("following a pointer re-bases its subtree", "[structs]")
{
    Structs             structs;
    const std::uint64_t player = structs.add_struct("Player", "1000");
    structs.add_field(player, "health", 0x10, integer(4));
    const std::uint64_t next  = structs.add_field(player, "next", 0x20, FieldType {FieldKind::pointer});
    const std::uint64_t value = structs.add_field(next, "value", 8, integer(4));

    const std::vector<std::string_view> names {"Player"};
    const std::vector<std::uint64_t>    bases {0x1000};

    std::vector<std::uint64_t> followed(structs.size(), 0);
    const auto                 unfollowed = place(structs, names, bases, followed, 8);
    CHECK(placement_of(unfollowed, structs, value).address == 0);

    followed[structs.index_of(next)] = 0x5000;
    const auto followed_placement    = place(structs, names, bases, followed, 8);
    CHECK(placement_of(followed_placement, structs, next).address == 0x1020);
    CHECK(placement_of(followed_placement, structs, value).address == 0x5008);
    CHECK(placement_of(followed_placement, structs, value).size == 4);

    const std::vector<std::uint8_t> visible(structs.size(), true);
    const ReadPlan                  plan = plan_reads(structs, followed_placement, visible);
    CHECK(plan.ranges
          == std::vector<std::pair<std::uint64_t, std::size_t>> {
              {0x1010, 4},
              {0x1020, 8},
              {0x5008, 4}
    });
}

TEST_CASE("an unresolved root stays at zero and is not read", "[structs]")
{
    Structs             structs;
    const std::uint64_t player = structs.add_struct("Player", "missing+0x10");
    const std::uint64_t health = structs.add_field(player, "health", 0x10, integer(4));

    const std::vector<std::string_view> names {"Player"};
    const std::vector<std::uint64_t>    bases {0};
    const std::vector<std::uint64_t>    followed(structs.size(), 0);

    const auto placements = place(structs, names, bases, followed, 8);
    CHECK(placement_of(placements, structs, player).address == 0);
    CHECK(placement_of(placements, structs, health).address == 0);

    std::vector<std::uint8_t> visible(structs.size(), true);
    CHECK(plan_reads(structs, placements, visible).ranges.empty());
}

TEST_CASE("plan_reads honours the visibility mask", "[structs]")
{
    Structs             structs;
    const std::uint64_t player = structs.add_struct("Player", "1000");
    const std::uint64_t health = structs.add_field(player, "health", 0x10, integer(4));
    const std::uint64_t armor  = structs.add_field(player, "armor", 0x14, integer(4));

    const std::vector<std::string_view> names {"Player"};
    const std::vector<std::uint64_t>    bases {0x1000};
    const std::vector<std::uint64_t>    followed(structs.size(), 0);
    const auto                          placements = place(structs, names, bases, followed, 8);

    std::vector<std::uint8_t> visible(structs.size(), true);
    visible[structs.index_of(armor)] = false;
    const ReadPlan plan              = plan_reads(structs, placements, visible);
    CHECK(plan.ranges
          == std::vector<std::pair<std::uint64_t, std::size_t>> {
              {0x1010, 4}
    });
    CHECK(plan.ids == std::vector<std::uint64_t> {health});
}

TEST_CASE("two roots are placed independently", "[structs]")
{
    Structs             structs;
    const std::uint64_t first  = structs.add_struct("First", "1000");
    const std::uint64_t a      = structs.add_field(first, "a", 8, integer(4));
    const std::uint64_t second = structs.add_struct("Second", "2000");
    const std::uint64_t b      = structs.add_field(second, "b", 0x10, integer(4));

    const std::vector<std::string_view> names {"First", "Second"};
    const std::vector<std::uint64_t>    bases {0x1000, 0x2000};
    const std::vector<std::uint64_t>    followed(structs.size(), 0);

    const auto placements = place(structs, names, bases, followed, 8);
    CHECK(placement_of(placements, structs, a).address == 0x1008);
    CHECK(placement_of(placements, structs, b).address == 0x2010);
}
