#include <catch2/catch.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "structs/types.hpp"
#include "support/struct_document_helpers.hpp"

namespace
{
    using slopkit::structs::FieldKind;
    using slopkit::structs::FieldType;

    struct Player
    {
        std::uint64_t root {};
        std::uint64_t health {};
        std::uint64_t speed {};
        std::uint64_t big {};
        std::uint64_t small {};
        std::uint64_t ratio {};
        std::uint64_t flags {};
        std::uint64_t name {};
        std::uint64_t next {};
    };

    Player build_player(Structs& definitions)
    {
        Player player;
        player.root = definitions.add_struct("Player", "0x1800");

        FieldType health;
        health.kind   = FieldKind::integer;
        health.width  = 4;
        player.health = definitions.add_field(player.root, "health", 0x10, health);

        FieldType speed;
        speed.kind   = FieldKind::floating;
        speed.width  = 4;
        player.speed = definitions.add_field(player.root, "speed", 0x14, speed);

        FieldType big;
        big.kind   = FieldKind::integer;
        big.width  = 8;
        player.big = definitions.add_field(player.root, "big", 0x18, big);

        FieldType small;
        small.kind      = FieldKind::integer;
        small.width     = 1;
        small.is_signed = false;
        player.small    = definitions.add_field(player.root, "small", 0x20, small);

        FieldType ratio;
        ratio.kind   = FieldKind::floating;
        ratio.width  = 8;
        player.ratio = definitions.add_field(player.root, "ratio", 0x28, ratio);

        FieldType flags;
        flags.kind   = FieldKind::array;
        flags.width  = 2;
        flags.count  = 4;
        player.flags = definitions.add_field(player.root, "flags", 0x30, flags);

        FieldType name;
        name.kind   = FieldKind::text;
        name.count  = 8;
        player.name = definitions.add_field(player.root, "name", 0x40, name);

        player.next = definitions.add_field(player.root, "next", 0x50, FieldType {FieldKind::pointer});
        return player;
    }

    void fill_player(FakeMemory& memory)
    {
        put_u32(memory, kStructBase + 0x10, static_cast<std::uint32_t>(static_cast<std::int32_t>(-1234)));
        put_f32(memory, kStructBase + 0x14, 1.5F);
        put_u64(memory, kStructBase + 0x18, 1000);
        memory.bytes[kStructBase + 0x20] = std::byte {0xA5};
        put_f64(memory, kStructBase + 0x28, 3.14159265);
        for (std::uint64_t index = 0; index < 4; ++index)
        {
            put_u16(memory, kStructBase + 0x30 + index * 2, static_cast<std::uint16_t>(index + 1));
        }
        put_text(memory, kStructBase + 0x40, "Hello");
        put_u64(memory, kStructBase + 0x50, 0x3000);
    }
}

TEST_CASE("the struct document decodes every field kind", "[ui]")
{
    StructFixture fixture;
    const Player  player = build_player(fixture.definitions);
    fill_player(*fixture.access.memory);

    fixture.pass();

    CHECK(fixture.document.value_text(player.root) == QStringLiteral("0x1800"));
    CHECK(fixture.document.value_text(player.health) == QStringLiteral("-1234"));
    CHECK(fixture.document.value_text(player.speed) == QStringLiteral("1.5"));
    CHECK(fixture.document.value_text(player.big) == QStringLiteral("1000"));
    CHECK(fixture.document.value_text(player.small) == QStringLiteral("165"));
    CHECK(fixture.document.value_text(player.ratio) == QStringLiteral("3.14159265"));
    CHECK(fixture.document.value_text(player.flags) == QStringLiteral("[1, 2, 3, 4]"));
    CHECK(fixture.document.value_text(player.name) == QStringLiteral("Hello"));
    CHECK(fixture.document.value_text(player.next) == QStringLiteral("0x3000"));

    CHECK(fixture.document.type_text(player.health) == QStringLiteral("int32"));
    CHECK(fixture.document.type_text(player.small) == QStringLiteral("uint8"));
    CHECK(fixture.document.type_text(player.ratio) == QStringLiteral("double"));
    CHECK(fixture.document.type_text(player.flags) == QStringLiteral("int16[4]"));
    CHECK(fixture.document.type_text(player.name) == QStringLiteral("char[8]"));
    CHECK(fixture.document.type_text(player.next) == QStringLiteral("void*"));
    CHECK(fixture.document.type_text(player.root).isEmpty());

    CHECK(fixture.document.offset_text(player.health) == QStringLiteral("10"));
    CHECK(fixture.document.offset_text(player.name) == QStringLiteral("40"));
    CHECK(fixture.document.offset_text(player.root).isEmpty());

    CHECK(fixture.document.describe(std::vector<std::uint64_t> {player.root})
          == QStringLiteral("Player at 0x1800, size 0x58"));
}

TEST_CASE("the struct document renders a hex field in hex", "[ui]")
{
    StructFixture fixture;
    const Player  player = build_player(fixture.definitions);
    fill_player(*fixture.access.memory);
    fixture.pass();

    CHECK(fixture.definitions.set_hex(player.small, true));
    CHECK(fixture.document.value_text(player.small) == QStringLiteral("0xA5"));
}

TEST_CASE("an unreadable field reads ? without disturbing the rest", "[ui]")
{
    StructFixture fixture;
    const Player  player = build_player(fixture.definitions);
    fill_player(*fixture.access.memory);

    const auto                            requests = fixture.document.next_live_request();
    std::vector<slopkit::ui::LiveReading> readings;
    readings.reserve(requests.size());
    for (const slopkit::ui::LiveRequest& request : requests)
    {
        if (request.address == kStructBase + 0x10)
        {
            readings.push_back(slopkit::ui::LiveReading {.id = request.id, .readable = false, .bytes = {}});
        }
        else
        {
            readings.push_back(reading(request.id, *fixture.access.memory, request.address, request.size));
        }
    }
    fixture.document.apply_live_readings(readings);

    CHECK(fixture.document.value_text(player.health) == QStringLiteral("?"));
    CHECK(fixture.document.value_text(player.big) == QStringLiteral("1000"));
}

TEST_CASE("a bracketed root resolves through the access worker", "[ui]")
{
    StructFixture       fixture;
    const std::uint64_t root   = fixture.definitions.add_struct("Player", "[0x2000]");
    const std::uint64_t health = fixture.definitions.add_field(root, "health", 0x8, FieldType {FieldKind::integer, 4});

    put_u64(*fixture.access.memory, 0x2000, 0x3000);
    put_u32(*fixture.access.memory, 0x3008, static_cast<std::uint32_t>(static_cast<std::int32_t>(-7)));

    const auto first = fixture.document.next_live_request();
    CHECK(first.empty());

    CHECK(fixture.pump(
        [&]
        {
            return fixture.document.value_text(root) == QStringLiteral("0x3000");
        }));
    fixture.pass();
    CHECK(fixture.document.value_text(health) == QStringLiteral("-7"));
}

TEST_CASE("the struct document stays quiet while hidden or before a pass", "[ui]")
{
    StructFixture fixture;
    const Player  player = build_player(fixture.definitions);
    fill_player(*fixture.access.memory);

    CHECK(fixture.document.value_text(player.health).isEmpty());

    fixture.document.set_visible(false);
    CHECK(fixture.document.next_live_request().empty());
    fixture.document.set_visible(true);
    CHECK_FALSE(fixture.document.next_live_request().empty());
}

TEST_CASE("writing a field submits one access-worker write", "[ui]")
{
    StructFixture fixture;
    const Player  player = build_player(fixture.definitions);
    fill_player(*fixture.access.memory);
    fixture.pass();

    int  statuses   = 0;
    bool last_error = true;
    QObject::connect(&fixture.document,
                     &StructDocument::statusChanged,
                     [&](const QString&, bool is_error)
                     {
                         ++statuses;
                         last_error = is_error;
                     });

    CHECK(fixture.document.write_value(player.health, QStringLiteral("-42")));
    CHECK(fixture.pump(
        [&]
        {
            return statuses == 1;
        }));

    CHECK_FALSE(last_error);
    CHECK(fixture.document.value_text(player.health) == QStringLiteral("-42"));
    CHECK(fixture.access.memory->bytes[kStructBase + 0x10] == std::byte {0xD6});
    CHECK(fixture.access.memory->bytes[kStructBase + 0x11] == std::byte {0xFF});
}

TEST_CASE("a non-pointer field cannot be followed", "[ui]")
{
    StructFixture       fixture;
    const std::uint64_t root   = fixture.definitions.add_struct("Player", "0x1800");
    const std::uint64_t health = fixture.definitions.add_field(root, "health", 0, FieldType {FieldKind::integer, 4});

    CHECK_FALSE(fixture.document.can_follow(health));
    CHECK_FALSE(fixture.document.follow_refusal(health).isEmpty());
    fixture.document.set_followed(health, true);
    CHECK_FALSE(fixture.document.followed(health));
}

TEST_CASE("following a pointer re-bases its children", "[ui]")
{
    StructFixture       fixture;
    const std::uint64_t player = fixture.definitions.add_struct("Player", "0x1800");
    const std::uint64_t next   = fixture.definitions.add_field(player, "next", 0x20, FieldType {FieldKind::pointer});
    const std::uint64_t value  = fixture.definitions.add_field(next, "value", 0x8, FieldType {FieldKind::integer, 4});

    put_u64(*fixture.access.memory, 0x1820, 0x3000);
    put_u32(*fixture.access.memory, 0x3008, static_cast<std::uint32_t>(static_cast<std::int32_t>(-5)));

    fixture.pass();
    CHECK(fixture.document.value_text(next) == QStringLiteral("0x3000"));
    CHECK(fixture.document.value_text(value).isEmpty());

    CHECK(fixture.document.can_follow(next));
    fixture.document.set_followed(next, true);
    fixture.pass();
    CHECK(fixture.document.followed(next));
    CHECK(fixture.document.value_text(value) == QStringLiteral("-5"));

    fixture.document.set_followed(next, false);
    fixture.pass();
    CHECK_FALSE(fixture.document.followed(next));
    CHECK(fixture.document.value_text(value).isEmpty());
}

TEST_CASE("following a pointer that loops back is refused", "[ui]")
{
    StructFixture       fixture;
    const std::uint64_t root = fixture.definitions.add_struct("Player", "0x1800");
    const std::uint64_t next = fixture.definitions.add_field(root, "next", 0x20, FieldType {FieldKind::pointer});
    fixture.definitions.add_field(next, "value", 0, FieldType {FieldKind::integer, 4});

    put_u64(*fixture.access.memory, 0x1820, 0x1800);
    fixture.pass();

    CHECK(fixture.document.value_text(next) == QStringLiteral("0x1800"));
    CHECK_FALSE(fixture.document.can_follow(next));
    CHECK_FALSE(fixture.document.follow_refusal(next).isEmpty());
    fixture.document.set_followed(next, true);
    CHECK_FALSE(fixture.document.followed(next));
}

TEST_CASE("writing is refused without a target", "[ui]")
{
    StructFixture fixture;
    const Player  player = build_player(fixture.definitions);
    fill_player(*fixture.access.memory);
    fixture.pass();

    fixture.target.session_live = false;

    int statuses = 0;
    QObject::connect(&fixture.document,
                     &StructDocument::statusChanged,
                     [&](const QString&, bool)
                     {
                         ++statuses;
                     });

    CHECK_FALSE(fixture.document.write_value(player.health, QStringLiteral("5")));
    CHECK(statuses == 1);
}
