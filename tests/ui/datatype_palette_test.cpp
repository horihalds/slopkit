#include <catch2/catch.hpp>

#include <array>
#include <string_view>

#include "support/ui_helpers.hpp"
#include "ui/datatype_palette.hpp"

namespace
{
    using slopkit::reclass::NodeKind;
    using slopkit::ui::datatype_colour;
    using slopkit::ui::datatype_icon_name;

    struct KindGlyph
    {
        NodeKind         kind;
        std::string_view stem;
    };

    constexpr KindGlyph kGlyphs[] = {
        {                NodeKind::int8,            "int8"},
        {               NodeKind::int16,           "int16"},
        {               NodeKind::int32,           "int32"},
        {               NodeKind::int64,           "int64"},
        {               NodeKind::uint8,           "uint8"},
        {              NodeKind::uint16,          "uint16"},
        {              NodeKind::uint32,          "uint32"},
        {              NodeKind::uint64,          "uint64"},
        {             NodeKind::float32,           "float"},
        {             NodeKind::float64,          "double"},
        {             NodeKind::boolean,            "bool"},
        {                NodeKind::hex8,            "hex8"},
        {               NodeKind::hex16,           "hex16"},
        {               NodeKind::hex32,           "hex32"},
        {               NodeKind::hex64,           "hex64"},
        {             NodeKind::pointer,         "pointer"},
        {               NodeKind::array,           "array"},
        {            NodeKind::bitfield,            "bits"},
        {           NodeKind::enum_node,     "enumeration"},
        {           NodeKind::utf8_text,        "utf8text"},
        {          NodeKind::utf16_text,       "utf16text"},
        {          NodeKind::utf32_text,       "utf32text"},
        {       NodeKind::utf8_text_ptr, "pointer_to_text"},
        {      NodeKind::utf16_text_ptr, "pointer_to_text"},
        {      NodeKind::utf32_text_ptr, "pointer_to_text"},
        {             NodeKind::vector2,         "vector2"},
        {             NodeKind::vector3,         "vector3"},
        {             NodeKind::vector4,         "vector4"},
        {           NodeKind::matrix3x3,       "matrix3x3"},
        {           NodeKind::matrix3x4,       "matrix3x4"},
        {           NodeKind::matrix4x4,       "matrix4x4"},
        {           NodeKind::character,                {}},
        {      NodeKind::class_instance,                {}},
        {       NodeKind::class_pointer,                {}},
        {         NodeKind::union_value,                {}},
        {NodeKind::class_instance_array,                {}},
        { NodeKind::class_wrapper_array,                {}},
        {                NodeKind::nint,                {}},
        {               NodeKind::nuint,                {}},
        {NodeKind::virtual_method_table,                {}},
        {      NodeKind::virtual_method,                {}},
        {            NodeKind::function,                {}},
        {        NodeKind::function_ptr,                {}},
    };
}

TEST_CASE("every kind resolves to its glyph or no glyph", "[ui]")
{
    application();

    for (const KindGlyph& entry : kGlyphs)
    {
        CHECK(datatype_icon_name(entry.kind) == entry.stem);

        const QIcon icon = slopkit::ui::widgets::datatype_icon(entry.kind);
        if (entry.stem.empty())
        {
            CHECK(icon.isNull());
        }
        else
        {
            CHECK_FALSE(icon.isNull());
            CHECK(icon.availableSizes().contains(QSize(16, 16)));
        }
    }
}

TEST_CASE("the datatype colours come from the active theme", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    const auto theme = slopkit::ui::dark_theme();

    constexpr std::array signed_kinds {
        NodeKind::nint, NodeKind::int8, NodeKind::int16, NodeKind::int32, NodeKind::int64};
    for (const auto kind : signed_kinds)
    {
        CHECK(datatype_colour(kind) == theme.datatype_signed);
    }

    constexpr std::array unsigned_kinds {
        NodeKind::nuint, NodeKind::uint8, NodeKind::uint16, NodeKind::uint32, NodeKind::uint64};
    for (const auto kind : unsigned_kinds)
    {
        CHECK(datatype_colour(kind) == theme.datatype_unsigned);
    }

    CHECK(datatype_colour(NodeKind::bitfield) == theme.datatype_bits);

    constexpr std::array float_kinds {NodeKind::float32, NodeKind::float64, NodeKind::boolean};
    for (const auto kind : float_kinds)
    {
        CHECK(datatype_colour(kind) == theme.datatype_float);
    }

    constexpr std::array vector_kinds {NodeKind::vector2,
                                       NodeKind::vector3,
                                       NodeKind::vector4,
                                       NodeKind::matrix3x3,
                                       NodeKind::matrix3x4,
                                       NodeKind::matrix4x4};
    for (const auto kind : vector_kinds)
    {
        CHECK(datatype_colour(kind) == theme.datatype_vector);
    }

    constexpr std::array text_kinds {NodeKind::utf8_text,
                                     NodeKind::utf8_text_ptr,
                                     NodeKind::utf16_text,
                                     NodeKind::utf16_text_ptr,
                                     NodeKind::utf32_text,
                                     NodeKind::utf32_text_ptr};
    for (const auto kind : text_kinds)
    {
        CHECK(datatype_colour(kind) == theme.datatype_text);
    }

    CHECK(datatype_colour(NodeKind::class_instance) == theme.datatype_class);
    CHECK(datatype_colour(NodeKind::class_pointer) == theme.datatype_class);

    constexpr std::array default_kinds {NodeKind::hex8,
                                        NodeKind::hex16,
                                        NodeKind::hex32,
                                        NodeKind::hex64,
                                        NodeKind::pointer,
                                        NodeKind::array,
                                        NodeKind::union_value,
                                        NodeKind::class_instance_array,
                                        NodeKind::class_wrapper_array,
                                        NodeKind::enum_node,
                                        NodeKind::character,
                                        NodeKind::virtual_method_table,
                                        NodeKind::virtual_method,
                                        NodeKind::function,
                                        NodeKind::function_ptr};
    for (const auto kind : default_kinds)
    {
        CHECK(datatype_colour(kind) == theme.text);
    }
}
