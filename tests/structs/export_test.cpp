#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

#include "structs/definition.hpp"
#include "structs/export.hpp"
#include "structs/types.hpp"

namespace
{
    using slopkit::structs::FieldKind;
    using slopkit::structs::FieldType;
    using slopkit::structs::render_cpp;
    using slopkit::structs::Structs;

    FieldType integer(std::size_t width, bool is_signed)
    {
        FieldType type;
        type.kind      = FieldKind::integer;
        type.width     = width;
        type.is_signed = is_signed;
        return type;
    }
}

TEST_CASE("render_cpp emits padding, arrays, text, pointers and nesting", "[structs]")
{
    Structs structs;

    const std::uint64_t point = structs.add_struct("Point", "1000");
    structs.add_field(point, "x", 0, integer(4, true));
    structs.add_field(point, "y", 4, integer(4, true));

    const std::uint64_t player = structs.add_struct("Player", "2000");
    structs.add_field(player, "health", 0x10, integer(4, true));
    structs.add_field(player, "speed", 0x14, FieldType {FieldKind::floating, 4});

    FieldType name;
    name.kind  = FieldKind::text;
    name.count = 8;
    structs.add_field(player, "name", 0x18, name);

    FieldType flags;
    flags.kind      = FieldKind::array;
    flags.width     = 2;
    flags.count     = 4;
    flags.is_signed = false;
    structs.add_field(player, "flags", 0x20, flags);

    structs.add_field(player, "next", 0x28, FieldType {FieldKind::pointer});

    FieldType origin {FieldKind::structure};
    origin.structure = "Point";
    structs.add_field(player, "origin", 0x30, origin);

    const std::string expected = "struct Point;\n"
                                 "struct Player;\n"
                                 "\n"
                                 "struct Point\n"
                                 "{\n"
                                 "    int32_t x;\n"
                                 "    int32_t y;\n"
                                 "}; // size 0x8\n"
                                 "\n"
                                 "struct Player\n"
                                 "{\n"
                                 "    uint8_t pad_0x0[16];\n"
                                 "    int32_t health;\n"
                                 "    float speed;\n"
                                 "    char name[8];\n"
                                 "    uint16_t flags[4];\n"
                                 "    void* next;\n"
                                 "    Point origin;\n"
                                 "}; // size 0x38\n"
                                 "\n";
    CHECK(render_cpp(structs, 8) == expected);
}

TEST_CASE("render_cpp flags overlapping fields", "[structs]")
{
    Structs             structs;
    const std::uint64_t root = structs.add_struct("Broken", "1000");
    structs.add_field(root, "a", 0x10, integer(4, true));
    structs.add_field(root, "b", 0x12, integer(4, true));

    const std::string output = render_cpp(structs, 8);
    CHECK(output.starts_with("struct Broken;\n\n// warning: Broken has fields that overlap\nstruct Broken\n"));
    CHECK(output.ends_with("}; // size 0x18\n\n"));
}

TEST_CASE("render_cpp renders an empty definition set as nothing", "[structs]")
{
    Structs structs;
    CHECK(render_cpp(structs, 8).empty());
}

TEST_CASE("render_cpp spells a typed pointer and an inner pad", "[structs]")
{
    Structs             structs;
    const std::uint64_t root = structs.add_struct("Node", "1000");
    structs.add_field(root, "value", 0, integer(8, false));

    FieldType link {FieldKind::pointer};
    link.structure = "Node";
    structs.add_field(root, "link", 0x10, link);

    const std::string output = render_cpp(structs, 8);
    CHECK(output.find("    uint64_t value;\n") != std::string::npos);
    CHECK(output.find("    uint8_t pad_0x8[8];\n") != std::string::npos);
    CHECK(output.find("    Node* link;\n") != std::string::npos);
    CHECK(output.ends_with("}; // size 0x18\n\n"));
}
