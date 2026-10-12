#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "structs/definition.hpp"
#include "structs/types.hpp"

namespace
{
    using slopkit::structs::FieldKind;
    using slopkit::structs::FieldType;
    using slopkit::structs::StructNode;
    using slopkit::structs::Structs;

    FieldType integer(std::size_t width, bool is_signed)
    {
        FieldType type;
        type.kind      = FieldKind::integer;
        type.width     = width;
        type.is_signed = is_signed;
        return type;
    }

    FieldType text(std::size_t count, bool wide)
    {
        FieldType type;
        type.kind  = FieldKind::text;
        type.count = count;
        type.wide  = wide;
        return type;
    }
}

TEST_CASE("fields nest under their struct and keep pre-order", "[structs]")
{
    Structs             structs;
    const std::uint64_t player = structs.add_struct("Player", "game.exe+0x1000");
    REQUIRE(player != 0);

    const std::uint64_t health = structs.add_field(player, "health", 0x10, integer(4, true));
    const std::uint64_t name   = structs.add_field(player, "name", 0x18, text(0x20, false));
    const std::uint64_t next   = structs.add_field(player, "next", 0x38, FieldType {FieldKind::pointer});

    REQUIRE(structs.size() == 4);
    CHECK(structs.nodes()[0].id == player);
    CHECK(structs.nodes()[0].expression == "game.exe+0x1000");
    CHECK(structs.nodes()[1].id == health);
    CHECK(structs.nodes()[2].id == name);
    CHECK(structs.nodes()[3].id == next);

    CHECK(structs.depth_of(0) == 0);
    CHECK(structs.depth_of(1) == 1);
    CHECK(structs.depth_of(2) == 1);
    CHECK(structs.depth_of(3) == 1);

    CHECK(structs.index_of(player) == 0);
    CHECK(structs.index_of(name) == 2);
    CHECK(structs.index_of(99) == structs.size());
    REQUIRE(structs.find(health) != nullptr);
    CHECK(structs.find(health)->offset == 0x10);
}

TEST_CASE("adding a field inserts it inside its parent's subtree", "[structs]")
{
    Structs             structs;
    const std::uint64_t first  = structs.add_struct("First", "1000");
    const std::uint64_t second = structs.add_struct("Second", "2000");

    const std::uint64_t a = structs.add_field(first, "a", 0, integer(4, true));
    const std::uint64_t b = structs.add_field(first, "b", 4, integer(4, true));

    REQUIRE(structs.size() == 4);
    CHECK(structs.nodes()[0].id == first);
    CHECK(structs.nodes()[1].id == a);
    CHECK(structs.nodes()[2].id == b);
    CHECK(structs.nodes()[3].id == second);
    CHECK(structs.root_of(2) == first);
    CHECK(structs.root_of(3) == second);
}

TEST_CASE("embedding puts fields a level deeper", "[structs]")
{
    Structs             structs;
    const std::uint64_t inner = structs.add_struct("Inner", "1000");
    structs.add_field(inner, "x", 0, integer(4, true));

    const std::uint64_t outer = structs.add_struct("Outer", "2000");
    FieldType           embedded {FieldKind::structure};
    embedded.structure      = "Inner";
    const std::uint64_t box = structs.add_field(outer, "inner", 0x20, embedded);
    structs.add_field(box, "y", 4, integer(4, true));

    CHECK(structs.size() == 5);
    CHECK(structs.depth_of(0) == 0);
    CHECK(structs.depth_of(1) == 1);
    CHECK(structs.depth_of(2) == 0);
    CHECK(structs.depth_of(3) == 1);
    CHECK(structs.depth_of(4) == 2);
    CHECK(structs.find_struct("Inner") != nullptr);
    CHECK(structs.find_struct("Missing") == nullptr);
}

TEST_CASE("struct_size spans the furthest field and rounds to the pointer size", "[structs]")
{
    Structs             structs;
    const std::uint64_t player = structs.add_struct("Player", "1000");
    structs.add_field(player, "health", 0x10, integer(4, true));
    structs.add_field(player, "speed", 0x14, FieldType {FieldKind::floating, 4});
    structs.add_field(player, "name", 0x18, text(8, false));
    structs.add_field(player, "next", 0x20, FieldType {FieldKind::pointer});

    CHECK(structs.struct_size(player, 8) == 0x28);

    FieldType array;
    array.kind  = FieldKind::array;
    array.width = 2;
    array.count = 4;
    structs.add_field(player, "flags", 0x28, array);
    CHECK(structs.struct_size(player, 8) == 0x30);

    Structs             bytes;
    const std::uint64_t tiny = bytes.add_struct("Tiny", "1000");
    bytes.add_field(tiny, "flag", 0, integer(1, false));
    CHECK(bytes.struct_size(tiny, 8) == 8);
    CHECK(bytes.struct_size(tiny, 4) == 4);
}

TEST_CASE("struct_size follows an embedded struct definition", "[structs]")
{
    Structs             structs;
    const std::uint64_t point = structs.add_struct("Point", "1000");
    structs.add_field(point, "x", 0, integer(4, true));
    structs.add_field(point, "y", 4, integer(4, true));

    const std::uint64_t holder = structs.add_struct("Holder", "2000");
    FieldType           embedded {FieldKind::structure};
    embedded.structure = "Point";
    structs.add_field(holder, "origin", 0x10, embedded);

    CHECK(structs.struct_size(point, 8) == 8);
    CHECK(structs.struct_size(holder, 8) == 0x18);
}

TEST_CASE("removing a struct erases its whole subtree", "[structs]")
{
    Structs             structs;
    const std::uint64_t first  = structs.add_struct("First", "1000");
    const std::uint64_t child  = structs.add_field(first, "child", 0, integer(4, true));
    const std::uint64_t nested = structs.add_field(child, "deep", 0, integer(4, true));
    const std::uint64_t second = structs.add_struct("Second", "2000");
    structs.add_field(second, "other", 0, integer(4, true));

    CHECK(structs.remove(first));
    CHECK(structs.size() == 2);
    CHECK(structs.find(first) == nullptr);
    CHECK(structs.find(child) == nullptr);
    CHECK(structs.find(nested) == nullptr);
    CHECK(structs.nodes()[0].id == second);

    CHECK_FALSE(structs.remove(12345));
    structs.clear();
    CHECK(structs.empty());
    CHECK(structs.root_ids().empty());
}

TEST_CASE("editing a node keeps the rest intact", "[structs]")
{
    Structs             structs;
    const std::uint64_t root  = structs.add_struct("Player", "1000");
    const std::uint64_t field = structs.add_field(root, "health", 0x10, integer(4, true));

    CHECK(structs.rename(field, "hp"));
    CHECK(structs.find(field)->name == "hp");
    CHECK(structs.set_offset(field, 0x1C));
    CHECK(structs.find(field)->offset == 0x1C);
    CHECK(structs.set_type(field, integer(8, false)));
    CHECK(structs.find(field)->type.width == 8);
    CHECK_FALSE(structs.find(field)->type.is_signed);
    CHECK(structs.set_expression(root, "game.exe+0x2000"));
    CHECK(structs.find(root)->expression == "game.exe+0x2000");
    CHECK(structs.set_hex(field, true));
    CHECK(structs.find(field)->hex);

    CHECK_FALSE(structs.rename(98765, "x"));
    CHECK_FALSE(structs.set_offset(98765, 0));
    CHECK_FALSE(structs.set_type(98765, integer(4, true)));
    CHECK_FALSE(structs.set_expression(98765, "x"));
    CHECK_FALSE(structs.set_hex(98765, false));
}

TEST_CASE("struct_names lists the roots in order", "[structs]")
{
    Structs structs;
    structs.add_struct("B", "2000");
    structs.add_struct("A", "1000");

    CHECK(structs.struct_names() == std::vector<std::string> {"B", "A"});
    CHECK(structs.root_ids().size() == 2);
}

TEST_CASE("field sizes follow the kind and pointer width", "[structs]")
{
    CHECK(integer(1, false).size(8) == 1);
    CHECK(integer(4, true).size(8) == 4);
    CHECK(integer(8, true).size(8) == 8);
    CHECK(FieldType {FieldKind::floating, 4}.size(8) == 4);
    CHECK(FieldType {FieldKind::floating, 8}.size(8) == 8);
    CHECK(FieldType {FieldKind::pointer}.size(4) == 4);
    CHECK(FieldType {FieldKind::pointer}.size(8) == 8);
    CHECK(text(0x10, false).size(8) == 0x10);
    CHECK(text(0x10, true).size(8) == 0x20);

    FieldType array;
    array.kind  = FieldKind::array;
    array.width = 2;
    array.count = 5;
    CHECK(array.size(8) == 10);

    FieldType counted {FieldKind::integer};
    counted.count = 3;
    CHECK(counted.size(8) == 12);
}

TEST_CASE("describe renders the type for the column", "[structs]")
{
    CHECK(integer(4, true).describe() == "int32");
    CHECK(integer(2, false).describe() == "uint16");
    CHECK(FieldType {FieldKind::floating, 8}.describe() == "double");
    CHECK(FieldType {FieldKind::pointer}.describe() == "void*");

    FieldType pointer {FieldKind::pointer};
    pointer.structure = "Player";
    CHECK(pointer.describe() == "Player*");

    CHECK(text(0x20, false).describe() == "char[32]");
    CHECK(text(4, true).describe() == "char16_t[4]");

    FieldType array;
    array.kind  = FieldKind::array;
    array.width = 4;
    array.count = 4;
    CHECK(array.describe() == "int32[4]");
}
