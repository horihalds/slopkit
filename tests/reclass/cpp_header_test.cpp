#include <catch2/catch.hpp>

#include <cstddef>
#include <memory>
#include <string>

#include "reclass/cpp_header.hpp"
#include "reclass/node.hpp"
#include "reclass/nodes/container.hpp"
#include "reclass/nodes/number.hpp"
#include "reclass/nodes/text.hpp"
#include "reclass/project.hpp"
#include "support/reclass_helpers.hpp"

namespace
{
    using slopkit::reclass::BitFieldNode;
    using slopkit::reclass::Class;
    using slopkit::reclass::cpp_header;
    using slopkit::reclass::NodeKind;
    using slopkit::reclass::NodePtr;
    using slopkit::reclass::Project;
    using slopkit::reclass::UTF16TextNode;
    using slopkit::reclass::UTF32TextNode;
    using slopkit::reclass::UTF8TextNode;
    using slopkit::test::array_of;
    using slopkit::test::node_of;
    using slopkit::test::union_of;

    // The one class of a project, so a case can append its members to it.
    Class& first_class(Project& project, std::string name = "Holder")
    {
        project.add_class(std::move(name), "app+0x10");
        return project.classes().back();
    }

    // A class node that names a class or an enum.
    NodePtr named_node(Project& project, NodeKind kind, std::string class_name, std::string name = {})
    {
        NodePtr node     = node_of(project, kind, std::move(name));
        node->class_name = std::move(class_name);
        return node;
    }
}

TEST_CASE("the scalar kinds keep their platform type", "[reclass][cpp_header]")
{
    Project project;
    auto&   holder = first_class(project);

    holder.nodes.push_back(node_of(project, NodeKind::int8, "a"));
    holder.nodes.push_back(node_of(project, NodeKind::int16, "b"));
    holder.nodes.push_back(node_of(project, NodeKind::int32, "c"));
    holder.nodes.push_back(node_of(project, NodeKind::int64, "d"));
    holder.nodes.push_back(node_of(project, NodeKind::uint8, "e"));
    holder.nodes.push_back(node_of(project, NodeKind::uint16, "f"));
    holder.nodes.push_back(node_of(project, NodeKind::uint32, "g"));
    holder.nodes.push_back(node_of(project, NodeKind::uint64, "h"));
    holder.nodes.push_back(node_of(project, NodeKind::float32, "i"));
    holder.nodes.push_back(node_of(project, NodeKind::float64, "j"));
    holder.nodes.push_back(node_of(project, NodeKind::boolean, "k"));
    holder.nodes.push_back(node_of(project, NodeKind::character, "l"));
    holder.nodes.push_back(node_of(project, NodeKind::nint, "m"));
    holder.nodes.push_back(node_of(project, NodeKind::nuint, "n"));
    holder.nodes.push_back(node_of(project, NodeKind::pointer, "o"));
    holder.nodes.push_back(node_of(project, NodeKind::function_ptr, "p"));

    const std::string text = cpp_header(project);

    CHECK(text.contains("struct Holder  // 0x4c bytes\n{\n"
                        "    /* 0x00 */ int8_t a;\n"
                        "    /* 0x01 */ int16_t b;\n"
                        "    /* 0x03 */ int32_t c;\n"
                        "    /* 0x07 */ int64_t d;\n"
                        "    /* 0x0f */ uint8_t e;\n"
                        "    /* 0x10 */ uint16_t f;\n"
                        "    /* 0x12 */ uint32_t g;\n"
                        "    /* 0x16 */ uint64_t h;\n"
                        "    /* 0x1e */ float i;\n"
                        "    /* 0x22 */ double j;\n"
                        "    /* 0x2a */ bool k;\n"
                        "    /* 0x2b */ char l;\n"
                        "    /* 0x2c */ ptrdiff_t m;\n"
                        "    /* 0x34 */ size_t n;\n"
                        "    /* 0x3c */ void* o;\n"
                        "    /* 0x44 */ void* p;\n"
                        "};\n"));
    CHECK(text.contains("static_assert(sizeof(Holder) == 0x4c, \"Holder\");"));
}

TEST_CASE("the geometry kinds keep their helper type", "[reclass][cpp_header]")
{
    Project project;
    auto&   shapes = first_class(project, "Shapes");

    shapes.nodes.push_back(node_of(project, NodeKind::vector2, "a"));
    shapes.nodes.push_back(node_of(project, NodeKind::vector3, "b"));
    shapes.nodes.push_back(node_of(project, NodeKind::vector4, "c"));
    shapes.nodes.push_back(node_of(project, NodeKind::matrix3x3, "d"));
    shapes.nodes.push_back(node_of(project, NodeKind::matrix3x4, "e"));
    shapes.nodes.push_back(node_of(project, NodeKind::matrix4x4, "f"));

    const std::string text = cpp_header(project);

    CHECK(text.contains("    /* 0x00 */ Vector2 a;\n"
                        "    /* 0x08 */ Vector3 b;\n"
                        "    /* 0x14 */ Vector4 c;\n"
                        "    /* 0x24 */ Matrix3x3 d;\n"
                        "    /* 0x48 */ Matrix3x4 e;\n"
                        "    /* 0x78 */ Matrix4x4 f;\n"));
    CHECK(text.contains("static_assert(sizeof(Shapes) == 0xb8, \"Shapes\");"));
}

TEST_CASE("a text member carries its encoding", "[reclass][cpp_header]")
{
    Project project;
    auto&   messages = first_class(project, "Messages");

    NodePtr utf8                             = node_of(project, NodeKind::utf8_text, "u8");
    static_cast<UTF8TextNode&>(*utf8).length = 8;
    messages.nodes.push_back(std::move(utf8));

    NodePtr utf16                              = node_of(project, NodeKind::utf16_text, "u16");
    static_cast<UTF16TextNode&>(*utf16).length = 4;
    messages.nodes.push_back(std::move(utf16));

    NodePtr utf32                              = node_of(project, NodeKind::utf32_text, "u32");
    static_cast<UTF32TextNode&>(*utf32).length = 2;
    messages.nodes.push_back(std::move(utf32));

    messages.nodes.push_back(node_of(project, NodeKind::utf8_text_ptr, "p8"));
    messages.nodes.push_back(node_of(project, NodeKind::utf16_text_ptr, "p16"));
    messages.nodes.push_back(node_of(project, NodeKind::utf32_text_ptr, "p32"));

    const std::string text = cpp_header(project);

    CHECK(text.contains("    /* 0x00 */ char u8[0x8];\n"
                        "    /* 0x08 */ char16_t u16[0x4];\n"
                        "    /* 0x10 */ char32_t u32[0x2];\n"
                        "    /* 0x18 */ char* p8;\n"
                        "    /* 0x20 */ char16_t* p16;\n"
                        "    /* 0x28 */ char32_t* p32;\n"));
    CHECK(text.contains("static_assert(sizeof(Messages) == 0x30, \"Messages\");"));
}

TEST_CASE("an array member brackets its element count", "[reclass][cpp_header]")
{
    Project project;
    auto&   lists = first_class(project, "Lists");

    NodePtr items = array_of(project, 4, NodeKind::int32);
    items->name   = "items";
    lists.nodes.push_back(std::move(items));
    lists.nodes.push_back(node_of(project, NodeKind::int32, "tail"));

    const std::string text = cpp_header(project);

    CHECK(text.contains("    /* 0x00 */ int32_t items[0x4];\n"
                        "    /* 0x10 */ int32_t tail;\n"));
    CHECK(text.contains("static_assert(sizeof(Lists) == 0x14, \"Lists\");"));
}

TEST_CASE("an array whose elements differ falls back to its bytes", "[reclass][cpp_header]")
{
    Project project;
    auto&   mixed = first_class(project, "Mixed");

    auto array  = std::make_unique<slopkit::reclass::ArrayNode>();
    array->id   = project.next_node_id();
    array->name = "mixed";
    array->children_mutable().push_back(node_of(project, NodeKind::int32));
    array->children_mutable().push_back(node_of(project, NodeKind::int32));
    array->children_mutable().push_back(node_of(project, NodeKind::int8));
    mixed.nodes.push_back(std::move(array));

    const std::string text = cpp_header(project);

    CHECK(text.contains("    /* 0x00 */ uint8_t mixed[0x9];\n"));
    CHECK(text.contains("static_assert(sizeof(Mixed) == 0x9, \"Mixed\");"));
}

TEST_CASE("a union member lists its members at the union's address", "[reclass][cpp_header]")
{
    Project project;
    auto&   bag = first_class(project, "Bag");

    NodePtr value = union_of(project, {NodeKind::int32, NodeKind::float32});
    value->name   = "value";
    bag.nodes.push_back(std::move(value));
    bag.nodes.push_back(node_of(project, NodeKind::int8, "tail"));

    const std::string text = cpp_header(project);

    CHECK(text.contains("    /* 0x00 */ union\n"
                        "    {\n"
                        "        int32_t int32_0x0;\n"
                        "        float float_0x0;\n"
                        "    } value;\n"
                        "    /* 0x04 */ int8_t tail;\n"));
    CHECK(text.contains("static_assert(sizeof(Bag) == 0x5, \"Bag\");"));
}

TEST_CASE("a bit field keeps its width and its signedness", "[reclass][cpp_header]")
{
    Project project;
    auto&   flags = first_class(project, "Flags");

    NodePtr small                           = node_of(project, NodeKind::bitfield, "a");
    static_cast<BitFieldNode&>(*small).bits = 3;
    flags.nodes.push_back(std::move(small));

    NodePtr wide                                  = node_of(project, NodeKind::bitfield, "b");
    static_cast<BitFieldNode&>(*wide).bits        = 12;
    static_cast<BitFieldNode&>(*wide).signed_bits = true;
    flags.nodes.push_back(std::move(wide));

    flags.nodes.push_back(node_of(project, NodeKind::int8, "c"));

    const std::string text = cpp_header(project);

    CHECK(text.contains("    /* 0x00 */ uint8_t a : 3;\n"
                        "    /* 0x01 */ int16_t b : 12;\n"
                        "    /* 0x03 */ int8_t c;\n"));
    CHECK(text.contains("static_assert(sizeof(Flags) == 0x4, \"Flags\");"));
}

TEST_CASE("an enum member names the project enum", "[reclass][cpp_header]")
{
    Project project;
    project.add_enum("Kind", 4);

    auto& holder = first_class(project);
    holder.nodes.push_back(named_node(project, NodeKind::enum_node, "Kind", "kind"));
    holder.nodes.push_back(named_node(project, NodeKind::enum_node, "Gone", "gone"));

    const std::string text = cpp_header(project);

    CHECK(text.contains("    /* 0x00 */ Kind kind;\n"
                        "    /* 0x04 */ uint32_t gone;\n"));
}

TEST_CASE("class references keep the class name", "[reclass][cpp_header]")
{
    Project project;
    project.add_class("Point", "app+0x10");
    auto& point = project.classes()[0];
    point.nodes.push_back(node_of(project, NodeKind::int32, "x"));
    point.nodes.push_back(node_of(project, NodeKind::int32, "y"));

    auto& holder = first_class(project);
    holder.nodes.push_back(named_node(project, NodeKind::class_instance, "Point", "origin"));
    holder.nodes.push_back(named_node(project, NodeKind::class_pointer, "Point", "ptr"));
    holder.nodes.push_back(named_node(project, NodeKind::class_pointer, "Missing", "unknown"));
    holder.nodes.push_back(named_node(project, NodeKind::class_instance, "Missing", "missing"));

    NodePtr points = named_node(project, NodeKind::class_instance_array, "Point", "points");
    static_cast<slopkit::reclass::ClassInstanceArrayNode&>(*points).count = 3;
    holder.nodes.push_back(std::move(points));

    NodePtr row = named_node(project, NodeKind::class_wrapper_array, "Point", "row");
    static_cast<slopkit::reclass::ClassWrapperArrayNode&>(*row).count = 2;
    holder.nodes.push_back(std::move(row));

    holder.nodes.push_back(node_of(project, NodeKind::int32, "tail"));

    const std::string text = cpp_header(project);

    CHECK(text.contains("    /* 0x00 */ Point origin;\n"
                        "    /* 0x08 */ Point* ptr;\n"
                        "    /* 0x10 */ void* unknown;\n"
                        "    /* 0x18 */ // missing: class Missing has no size of its own\n"
                        "    /* 0x18 */ Point* points;\n"
                        "    /* 0x20 */ Point row[0x2];\n"
                        "    /* 0x30 */ int32_t tail;\n"));
    CHECK(text.contains("static_assert(sizeof(Holder) == 0x34, \"Holder\");"));
}

TEST_CASE("a run of hex members collapses into one byte block", "[reclass][cpp_header]")
{
    Project project;
    auto&   holder = first_class(project);

    holder.nodes.push_back(node_of(project, NodeKind::int32, "head"));
    holder.nodes.push_back(node_of(project, NodeKind::hex8));
    holder.nodes.push_back(node_of(project, NodeKind::hex8));
    holder.nodes.push_back(node_of(project, NodeKind::hex16));
    holder.nodes.push_back(node_of(project, NodeKind::int32, "tail"));

    const std::string text = cpp_header(project);

    CHECK(text.contains("    /* 0x00 */ int32_t head;\n"
                        "    /* 0x04 */ uint8_t pad_0x4[0x4];\n"
                        "    /* 0x08 */ int32_t tail;\n"));
    CHECK(text.contains("static_assert(sizeof(Holder) == 0xc, \"Holder\");"));
}

TEST_CASE("a named hex member lends its name to the run", "[reclass][cpp_header]")
{
    Project project;
    auto&   holder = first_class(project);

    holder.nodes.push_back(node_of(project, NodeKind::hex8, "reserved"));
    holder.nodes.push_back(node_of(project, NodeKind::hex32));

    const std::string text = cpp_header(project);

    CHECK(text.contains("    /* 0x00 */ uint8_t reserved[0x5];\n"));
}

TEST_CASE("a lone hex member keeps its own width", "[reclass][cpp_header]")
{
    Project project;
    auto&   holder = first_class(project);

    holder.nodes.push_back(node_of(project, NodeKind::int32, "head"));
    holder.nodes.push_back(node_of(project, NodeKind::hex16));
    holder.nodes.push_back(node_of(project, NodeKind::int32, "middle"));
    holder.nodes.push_back(node_of(project, NodeKind::hex64, "raw"));
    holder.nodes.push_back(node_of(project, NodeKind::int32, "tail"));

    const std::string text = cpp_header(project);

    CHECK(text.contains("    /* 0x00 */ int32_t head;\n"
                        "    /* 0x04 */ uint16_t unknown_0x4;\n"
                        "    /* 0x06 */ int32_t middle;\n"
                        "    /* 0x0a */ uint64_t raw;\n"
                        "    /* 0x12 */ int32_t tail;\n"));
}

TEST_CASE("an unnamed member is named after its kind and offset", "[reclass][cpp_header]")
{
    Project project;
    auto&   names = first_class(project, "Names");

    names.nodes.push_back(node_of(project, NodeKind::int32));
    names.nodes.push_back(node_of(project, NodeKind::vector3));
    names.nodes.push_back(node_of(project, NodeKind::pointer));

    const std::string text = cpp_header(project);

    CHECK(text.contains("    /* 0x00 */ int32_t int32_0x0;\n"
                        "    /* 0x04 */ Vector3 vector3_0x4;\n"
                        "    /* 0x10 */ void* pointer_0x10;\n"));
}

TEST_CASE("a name that is not an identifier is sanitized and deduplicated", "[reclass][cpp_header]")
{
    Project project;
    auto&   names = first_class(project, "Names");

    names.nodes.push_back(node_of(project, NodeKind::int32, "same name"));
    names.nodes.push_back(node_of(project, NodeKind::int32, "same-name"));
    names.nodes.push_back(node_of(project, NodeKind::int32, "class"));
    names.nodes.push_back(node_of(project, NodeKind::int32, "1st"));

    const std::string text = cpp_header(project);

    CHECK(text.contains("    /* 0x00 */ int32_t same_name;\n"
                        "    /* 0x04 */ int32_t same_name_1;\n"
                        "    /* 0x08 */ int32_t class_;\n"
                        "    /* 0x0c */ int32_t _1st;\n"));
}

TEST_CASE("a class name that is not an identifier falls back to its index", "[reclass][cpp_header]")
{
    Project project;
    project.classes().push_back(Class {});
    project.classes().push_back(Class {});

    project.classes()[0].name = "bad name!";
    project.classes()[0].nodes.push_back(node_of(project, NodeKind::int32));
    project.classes()[1].nodes.push_back(node_of(project, NodeKind::int32, "x"));

    const std::string text = cpp_header(project);

    CHECK(text.contains("struct bad_name_  // 0x4 bytes\n"));
    CHECK(text.contains("static_assert(sizeof(bad_name_) == 0x4, \"bad_name_\");"));
    CHECK(text.contains("struct Class_0x1  // 0x4 bytes\n"));
}

TEST_CASE("an empty project still carries the guards and the helper types", "[reclass][cpp_header]")
{
    Project project;

    const std::string text = cpp_header(project);

    CHECK(text.starts_with("// Generated by slopkit from the ReClass project. Do not edit.\n"
                           "#ifndef SLOPKIT_RECLASS_HEADER\n"
                           "#define SLOPKIT_RECLASS_HEADER\n"
                           "\n"
                           "#include <cstddef>\n"
                           "#include <cstdint>\n"));
    CHECK(text.ends_with("#endif\n\n#endif  // SLOPKIT_RECLASS_HEADER\n"));
    CHECK_FALSE(text.contains("#pragma pack"));
}

TEST_CASE("each helper type carries its own guard exactly once", "[reclass][cpp_header]")
{
    Project project;

    const std::string text = cpp_header(project);

    const std::string expected[] {
        "#ifndef VECTOR2_DEFINED\n#define VECTOR2_DEFINED\n"
        "struct Vector2 { float x, y; };\n"
        "static_assert(sizeof(Vector2) == 0x8);\n#endif\n",
        "#ifndef VECTOR3_DEFINED\n#define VECTOR3_DEFINED\n"
        "struct Vector3 { float x, y, z; };\n"
        "static_assert(sizeof(Vector3) == 0xc);\n#endif\n",
        "#ifndef VECTOR4_DEFINED\n#define VECTOR4_DEFINED\n"
        "struct Vector4 { float x, y, z, w; };\n"
        "static_assert(sizeof(Vector4) == 0x10);\n#endif\n",
        "#ifndef MATRIX3X3_DEFINED\n#define MATRIX3X3_DEFINED\n"
        "struct Matrix3x3 { float m[3][3]; };\n"
        "static_assert(sizeof(Matrix3x3) == 0x24);\n#endif\n",
        "#ifndef MATRIX3X4_DEFINED\n#define MATRIX3X4_DEFINED\n"
        "struct Matrix3x4 { float m[3][4]; };\n"
        "static_assert(sizeof(Matrix3x4) == 0x30);\n#endif\n",
        "#ifndef MATRIX4X4_DEFINED\n#define MATRIX4X4_DEFINED\n"
        "struct Matrix4x4 { float m[4][4]; };\n"
        "static_assert(sizeof(Matrix4x4) == 0x40);\n#endif\n",
    };

    for (const std::string& block : expected)
    {
        CHECK(text.find(block) != std::string::npos);
        CHECK(text.find(block, text.find(block) + 1) == std::string::npos);
    }
}

TEST_CASE("the whole document keeps the guard around the enums and the classes", "[reclass][cpp_header]")
{
    Project project;
    project.add_enum("Kind", 4);
    project.enums()[0].values.push_back({"None", 0, ""});
    project.enums()[0].values.push_back({"Some", 2, "the some value"});

    project.add_class("Point", "app+0x10");
    project.classes()[0].nodes.push_back(node_of(project, NodeKind::int32, "x"));
    project.classes()[0].nodes.push_back(node_of(project, NodeKind::int32, "y"));

    auto& holder = first_class(project, "Holder");
    holder.nodes.push_back(named_node(project, NodeKind::class_instance, "Point", "origin"));
    holder.nodes.push_back(named_node(project, NodeKind::class_pointer, "Point", "ptr"));

    const std::string text = cpp_header(project);

    CHECK(text.contains("enum class Kind : uint32_t\n"
                        "{\n"
                        "    None = 0x0,\n"
                        "    Some = 0x2,  // the some value\n"
                        "};\n"));
    CHECK(text.find("enum class Kind") < text.find("struct Point;"));

    CHECK(text.ends_with("struct Point;\n"
                         "struct Holder;\n"
                         "\n"
                         "#pragma pack(push, 1)\n"
                         "struct Point  // 0x8 bytes\n"
                         "{\n"
                         "    /* 0x00 */ int32_t x;\n"
                         "    /* 0x04 */ int32_t y;\n"
                         "};\n"
                         "static_assert(sizeof(Point) == 0x8, \"Point\");\n"
                         "\n"
                         "struct Holder  // 0x10 bytes\n"
                         "{\n"
                         "    /* 0x00 */ Point origin;\n"
                         "    /* 0x08 */ Point* ptr;\n"
                         "};\n"
                         "static_assert(sizeof(Holder) == 0x10, \"Holder\");\n"
                         "\n"
                         "#pragma pack(pop)\n"
                         "\n"
                         "#endif  // SLOPKIT_RECLASS_HEADER\n"));
}

TEST_CASE("an embedded class is defined before the class that holds it", "[reclass][cpp_header]")
{
    Project project;
    project.add_class("Outer", "app+0x10");
    project.add_class("Inner", "app+0x20");
    auto& outer = project.classes()[0];
    auto& inner = project.classes()[1];

    inner.nodes.push_back(node_of(project, NodeKind::int32, "value"));
    outer.nodes.push_back(named_node(project, NodeKind::class_instance, "Inner", "nested"));

    const std::string text = cpp_header(project);

    CHECK(text.contains("struct Inner;\nstruct Outer;\n"));
    CHECK(text.find("struct Inner  // 0x4 bytes") < text.find("struct Outer  // 0x4 bytes"));
    CHECK(text.ends_with("#endif  // SLOPKIT_RECLASS_HEADER\n"));
}

TEST_CASE("a class that contains itself does not stop the export", "[reclass][cpp_header]")
{
    Project project;
    auto&   self = first_class(project, "Self");
    self.nodes.push_back(named_node(project, NodeKind::class_instance, "Self", "self"));
    self.nodes.push_back(node_of(project, NodeKind::int32, "x"));

    const std::string text = cpp_header(project);

    CHECK(text.contains("struct Self  // the class contains itself by value\n"
                        "{\n"
                        "    /* 0x00 */ // self: class Self has no size of its own\n"
                        "    /* 0x00 */ int32_t x;\n"
                        "};\n"));
    CHECK_FALSE(text.contains("static_assert(sizeof(Self)"));
    CHECK(text.ends_with("#endif  // SLOPKIT_RECLASS_HEADER\n"));
}
