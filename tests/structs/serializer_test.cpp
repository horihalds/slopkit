#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "structs/definition.hpp"
#include "structs/serializer.hpp"
#include "structs/types.hpp"

namespace
{
    using slopkit::structs::FieldKind;
    using slopkit::structs::FieldType;
    using slopkit::structs::from_text;
    using slopkit::structs::Structs;
    using slopkit::structs::to_text;

    FieldType integer(std::size_t width, bool is_signed)
    {
        FieldType type;
        type.kind      = FieldKind::integer;
        type.width     = width;
        type.is_signed = is_signed;
        return type;
    }
}

TEST_CASE("to_text writes the root then the fields by depth", "[structs]")
{
    Structs             structs;
    const std::uint64_t player = structs.add_struct("Player", "game.exe+0x1A40");
    structs.add_field(player, "health", 0x10, integer(4, true));
    structs.add_field(player, "speed", 0x14, FieldType {FieldKind::floating, 4});

    FieldType text;
    text.kind  = FieldKind::text;
    text.count = 0x20;
    structs.add_field(player, "name", 0x18, text);

    FieldType next {FieldKind::pointer};
    next.structure = "Player";
    structs.add_field(player, "next", 0x38, next);

    const std::string expected = "struct name=\"Player\" expr=\"game.exe+0x1A40\"\n"
                                 "depth=1 name=\"health\" offset=0x10 type=i32 hex=0\n"
                                 "depth=1 name=\"speed\" offset=0x14 type=f32 hex=0\n"
                                 "depth=1 name=\"name\" offset=0x18 type=text count=0x20 wide=0 hex=0\n"
                                 "depth=1 name=\"next\" offset=0x38 type=ptr struct=\"Player\" hex=0\n";
    CHECK(to_text(structs, player) == expected);
}

TEST_CASE("a struct body round-trips through text", "[structs]")
{
    Structs             source;
    const std::uint64_t player = source.add_struct("Player", "game.exe+0x1A40");
    source.add_field(player, "health", 0x10, integer(4, true));
    source.add_field(player, "speed", 0x14, FieldType {FieldKind::floating, 8});

    FieldType text;
    text.kind  = FieldKind::text;
    text.count = 0x20;
    text.wide  = true;
    source.add_field(player, "banner", 0x18, text);

    FieldType array;
    array.kind                = FieldKind::array;
    array.width               = 2;
    array.count               = 4;
    const std::uint64_t flags = source.add_field(player, "flags", 0x40, array);
    source.set_hex(flags, true);

    FieldType embedded {FieldKind::structure};
    embedded.structure      = "Box";
    const std::uint64_t box = source.add_field(player, "box", 0x48, embedded);
    source.add_field(box, "x", 0, integer(8, false));

    const std::string body = to_text(source, player);

    Structs    loaded;
    const auto root = from_text(body, loaded);
    REQUIRE(root.has_value());

    const std::uint64_t loaded_root = *root;
    REQUIRE(loaded.size() == source.size());
    CHECK(loaded.find(loaded_root)->name == "Player");
    CHECK(loaded.find(loaded_root)->expression == "game.exe+0x1A40");

    CHECK(loaded.nodes()[1].name == "health");
    CHECK(loaded.nodes()[1].offset == 0x10);
    CHECK(loaded.nodes()[1].type.kind == FieldKind::integer);
    CHECK(loaded.nodes()[1].type.width == 4);
    CHECK(loaded.nodes()[1].type.is_signed);

    CHECK(loaded.nodes()[2].type.kind == FieldKind::floating);
    CHECK(loaded.nodes()[2].type.width == 8);

    CHECK(loaded.nodes()[3].type.kind == FieldKind::text);
    CHECK(loaded.nodes()[3].type.count == 0x20);
    CHECK(loaded.nodes()[3].type.wide);

    CHECK(loaded.nodes()[4].name == "flags");
    CHECK(loaded.nodes()[4].type.kind == FieldKind::array);
    CHECK(loaded.nodes()[4].type.width == 2);
    CHECK(loaded.nodes()[4].type.count == 4);
    CHECK(loaded.nodes()[4].hex);

    CHECK(loaded.nodes()[5].type.kind == FieldKind::structure);
    CHECK(loaded.nodes()[5].type.structure == "Box");
    CHECK(loaded.nodes()[6].offset == 0);
    CHECK(loaded.nodes()[6].type.width == 8);
    CHECK_FALSE(loaded.nodes()[6].type.is_signed);
    CHECK(loaded.depth_of(6) == 2);

    CHECK(to_text(loaded, loaded_root) == body);
}

TEST_CASE("names and expressions survive quoting", "[structs]")
{
    Structs             structs;
    const std::uint64_t root = structs.add_struct("Weird \"name\"", R"(base\path+0x10)");
    structs.add_field(root, "with\"quote", 0, integer(4, true));

    Structs    loaded;
    const auto parsed = from_text(to_text(structs, root), loaded);
    REQUIRE(parsed.has_value());
    CHECK(loaded.find(*parsed)->name == "Weird \"name\"");
    CHECK(loaded.find(*parsed)->expression == R"(base\path+0x10)");
    CHECK(loaded.nodes()[1].name == "with\"quote");
}

TEST_CASE("from_text rejects a malformed member", "[structs]")
{
    Structs structs;

    CHECK_FALSE(from_text("", structs).has_value());
    CHECK_FALSE(from_text("depth=1 name=\"x\" offset=0 type=i32 hex=0\n", structs).has_value());
    CHECK_FALSE(
        from_text("struct name=\"x\" expr=\"1000\"\nname=\"y\" offset=0 type=i32 hex=0\n", structs).has_value());
    CHECK_FALSE(from_text("struct name=\"x\" expr=\"1000\"\ndepth=1 name=\"y\" offset=0 type=zzz hex=0\n", structs)
                    .has_value());
    CHECK_FALSE(from_text("struct name=\"x\" expr=\"1000\"\ndepth=2 name=\"y\" offset=0 type=i32 hex=0\n", structs)
                    .has_value());
}

TEST_CASE("a root with no fields is still valid", "[structs]")
{
    Structs    structs;
    const auto root = from_text("struct name=\"Empty\" expr=\"1000\"\n", structs);
    REQUIRE(root.has_value());
    CHECK(structs.size() == 1);
    CHECK(structs.find(*root)->expression == "1000");
}
