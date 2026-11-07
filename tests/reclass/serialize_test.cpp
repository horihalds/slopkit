#include <catch2/catch.hpp>

#include <cstddef>
#include <initializer_list>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "reclass/enum.hpp"
#include "reclass/node.hpp"
#include "reclass/nodes/container.hpp"
#include "reclass/nodes/node.hpp"
#include "reclass/nodes/number.hpp"
#include "reclass/nodes/text.hpp"
#include "reclass/project.hpp"
#include "reclass/serialize.hpp"
#include "support/reclass_helpers.hpp"

namespace
{
    using slopkit::reclass::from_members;
    using slopkit::reclass::kMemberPrefix;
    using slopkit::reclass::Member;
    using slopkit::reclass::Node;
    using slopkit::reclass::NodeKind;
    using slopkit::reclass::Project;
    using slopkit::reclass::to_members;
    using slopkit::test::array_of;
    using slopkit::test::instance_array_of;
    using slopkit::test::node_of;
    using slopkit::test::union_of;
    using slopkit::test::wrapper_array_of;

    Project make_project()
    {
        Project project;
        project.add_class("Player", "game.exe+0x1234");
        project.add_class("Enemy", "game.exe+0x5678");

        auto& player = project.classes()[0];
        player.nodes.push_back(node_of(project, NodeKind::int32, "health"));
        player.nodes.back()->comment = "hit points";
        auto link                    = node_of(project, NodeKind::class_pointer, "next");
        link->class_name             = "Enemy";
        player.nodes.push_back(std::move(link));

        auto& enemy = project.classes()[1];
        enemy.nodes.push_back(node_of(project, NodeKind::float32, "x"));
        enemy.nodes.back()->hex = true;
        return project;
    }

    void require_same_node(const Node& expected, const Node& actual)
    {
        REQUIRE(actual.kind() == expected.kind());
        REQUIRE(actual.name == expected.name);
        REQUIRE(actual.comment == expected.comment);
        REQUIRE(actual.class_name == expected.class_name);
        REQUIRE(actual.hex == expected.hex);
        REQUIRE(actual.hidden == expected.hidden);

        if (const auto* bitfield = dynamic_cast<const slopkit::reclass::BitFieldNode*>(&expected); bitfield != nullptr)
        {
            const auto& actual_bitfield = static_cast<const slopkit::reclass::BitFieldNode&>(actual);
            REQUIRE(actual_bitfield.bits == bitfield->bits);
            REQUIRE(actual_bitfield.signed_bits == bitfield->signed_bits);
        }
        if (const auto* enum_node = dynamic_cast<const slopkit::reclass::EnumNode*>(&expected); enum_node != nullptr)
        {
            const auto& actual_enum = static_cast<const slopkit::reclass::EnumNode&>(actual);
            REQUIRE(actual_enum.enum_name == enum_node->enum_name);
            REQUIRE(actual_enum.size_bytes == enum_node->size_bytes);
        }
        if (const auto* text = dynamic_cast<const slopkit::reclass::UTF8TextNode*>(&expected); text != nullptr)
        {
            REQUIRE(static_cast<const slopkit::reclass::UTF8TextNode&>(actual).length == text->length);
        }
        if (const auto* text = dynamic_cast<const slopkit::reclass::UTF16TextNode*>(&expected); text != nullptr)
        {
            REQUIRE(static_cast<const slopkit::reclass::UTF16TextNode&>(actual).length == text->length);
        }
        if (const auto* text = dynamic_cast<const slopkit::reclass::UTF32TextNode*>(&expected); text != nullptr)
        {
            REQUIRE(static_cast<const slopkit::reclass::UTF32TextNode&>(actual).length == text->length);
        }
        if (const auto* instances = dynamic_cast<const slopkit::reclass::ClassInstanceArrayNode*>(&expected);
            instances != nullptr)
        {
            REQUIRE(static_cast<const slopkit::reclass::ClassInstanceArrayNode&>(actual).count == instances->count);
        }
        if (const auto* wrappers = dynamic_cast<const slopkit::reclass::ClassWrapperArrayNode*>(&expected);
            wrappers != nullptr)
        {
            REQUIRE(static_cast<const slopkit::reclass::ClassWrapperArrayNode&>(actual).count == wrappers->count);
        }

        const auto expected_children = expected.children();
        const auto actual_children   = actual.children();
        REQUIRE(actual_children.size() == expected_children.size());
        for (std::size_t index = 0; index < expected_children.size(); ++index)
        {
            require_same_node(*expected_children[index], *actual_children[index]);
        }
    }

    void require_same(const Project& left, const Project& right)
    {
        REQUIRE(left.classes().size() == right.classes().size());
        for (std::size_t index = 0; index < left.classes().size(); ++index)
        {
            const auto& expected = left.classes()[index];
            const auto& actual   = right.classes()[index];
            REQUIRE(actual.name == expected.name);
            REQUIRE(actual.address_expression == expected.address_expression);
            REQUIRE(actual.nodes.size() == expected.nodes.size());
            for (std::size_t position = 0; position < expected.nodes.size(); ++position)
            {
                require_same_node(*expected.nodes[position], *actual.nodes[position]);
            }
        }

        REQUIRE(left.enums().size() == right.enums().size());
        for (std::size_t index = 0; index < left.enums().size(); ++index)
        {
            const auto& expected = left.enums()[index];
            const auto& actual   = right.enums()[index];
            REQUIRE(actual.name == expected.name);
            REQUIRE(actual.size == expected.size);
            REQUIRE(actual.values.size() == expected.values.size());
            for (std::size_t position = 0; position < expected.values.size(); ++position)
            {
                REQUIRE(actual.values[position].name == expected.values[position].name);
                REQUIRE(actual.values[position].value == expected.values[position].value);
                REQUIRE(actual.values[position].comment == expected.values[position].comment);
            }
        }
    }
}

TEST_CASE("a class project round-trips through its members", "[reclass]")
{
    const Project original = make_project();

    const std::vector<Member> members = to_members(original);
    REQUIRE(members.size() == 3);
    REQUIRE(members.front().name == std::string(kMemberPrefix) + "index.txt");

    const auto restored = from_members(members);
    REQUIRE(restored.has_value());
    require_same(original, *restored);
    REQUIRE(restored->classes()[0].nodes[0]->id != 0);
    REQUIRE(restored->classes()[0].nodes[0]->id != restored->classes()[1].nodes[0]->id);
}

TEST_CASE("the class member text is the documented layout", "[reclass]")
{
    Project project;
    project.add_class("Player", "game.exe+0x1234");
    project.classes().front().nodes.push_back(node_of(project, NodeKind::int32, "health"));

    const std::vector<Member> members = to_members(project);
    REQUIRE(members.size() == 2);
    REQUIRE(members[0].name == "reclass/index.txt");
    REQUIRE(members[0].text == "v2\nreclass/classes/Player.txt\n");
    REQUIRE(members[1].name == "reclass/classes/Player.txt");
    REQUIRE(members[1].text
            == "v2\n"
               "name=\"Player\" address=\"game.exe+0x1234\"\n"
               "node depth=0 kind=i32 flags=0 name=\"health\" comment=\"\"\n");
}

TEST_CASE("a project with every kind round-trips", "[reclass]")
{
    Project project;
    project.add_class("Target", "app+0x1000");
    project.add_class("Holder", "app+0x2000");

    auto* state = project.add_enum("State", 4);
    REQUIRE(state != nullptr);
    state->values.push_back(slopkit::reclass::EnumValue {.name = "Idle", .value = 0, .comment = "resting"});
    state->values.push_back(slopkit::reclass::EnumValue {.name = "Run", .value = 1, .comment = {}});

    auto& holder = project.classes()[1];

    const std::vector<NodeKind> scalar_kinds {
        NodeKind::int8,           NodeKind::int16,          NodeKind::int32,
        NodeKind::int64,          NodeKind::uint8,          NodeKind::uint16,
        NodeKind::uint32,         NodeKind::uint64,         NodeKind::nint,
        NodeKind::nuint,          NodeKind::float32,        NodeKind::float64,
        NodeKind::boolean,        NodeKind::character,      NodeKind::hex8,
        NodeKind::hex16,          NodeKind::hex32,          NodeKind::hex64,
        NodeKind::pointer,        NodeKind::class_pointer,  NodeKind::utf8_text_ptr,
        NodeKind::utf16_text_ptr, NodeKind::utf32_text_ptr, NodeKind::vector2,
        NodeKind::vector3,        NodeKind::vector4,        NodeKind::matrix3x3,
        NodeKind::matrix3x4,      NodeKind::matrix4x4,      NodeKind::virtual_method_table,
        NodeKind::virtual_method, NodeKind::function,       NodeKind::function_ptr,
    };
    for (const NodeKind kind : scalar_kinds)
    {
        auto node    = node_of(project, kind, std::string(slopkit::reclass::type_token(kind)));
        node->hex    = slopkit::reclass::is_hex_kind(kind);
        node->hidden = kind == NodeKind::int8;
        if (kind == NodeKind::class_pointer)
        {
            node->class_name = "Target";
        }
        holder.nodes.push_back(std::move(node));
    }

    auto instance        = node_of(project, NodeKind::class_instance, "instance");
    instance->class_name = "Target";
    holder.nodes.push_back(std::move(instance));

    {
        auto node  = instance_array_of(project, "Target", 3);
        node->name = "instances";
        holder.nodes.push_back(std::move(node));
    }
    {
        auto node  = wrapper_array_of(project, "Target", 2);
        node->name = "wrappers";
        holder.nodes.push_back(std::move(node));
    }
    {
        auto node  = array_of(project, 4, NodeKind::int32);
        node->name = "items";
        holder.nodes.push_back(std::move(node));
    }
    {
        auto node  = union_of(project, {NodeKind::int32, NodeKind::float64});
        node->name = "value";
        holder.nodes.push_back(std::move(node));
    }
    {
        auto  node            = node_of(project, NodeKind::bitfield, "flags");
        auto* bitfield        = static_cast<slopkit::reclass::BitFieldNode*>(node.get());
        bitfield->bits        = 3;
        bitfield->signed_bits = true;
        holder.nodes.push_back(std::move(node));
    }
    {
        auto  node            = node_of(project, NodeKind::enum_node, "state");
        auto* enum_node       = static_cast<slopkit::reclass::EnumNode*>(node.get());
        enum_node->enum_name  = "State";
        enum_node->size_bytes = 4;
        holder.nodes.push_back(std::move(node));
    }
    {
        auto node = node_of(project, NodeKind::utf8_text, "hello");
        static_cast<slopkit::reclass::UTF8TextNode*>(node.get())->length = 12;
        holder.nodes.push_back(std::move(node));
    }
    {
        auto node = node_of(project, NodeKind::utf16_text, "wide");
        static_cast<slopkit::reclass::UTF16TextNode*>(node.get())->length = 8;
        holder.nodes.push_back(std::move(node));
    }
    {
        auto node = node_of(project, NodeKind::utf32_text, "wide32");
        static_cast<slopkit::reclass::UTF32TextNode*>(node.get())->length = 4;
        holder.nodes.push_back(std::move(node));
    }

    const auto restored = from_members(to_members(project));
    REQUIRE(restored.has_value());
    require_same(project, *restored);
    REQUIRE(restored->enums().size() == 1);
    REQUIRE(restored->enums()[0].name == "State");
    REQUIRE(restored->enums()[0].size == 4);
    REQUIRE(restored->enums()[0].values.size() == 2);
    REQUIRE(restored->enums()[0].values[0].name == "Idle");
    REQUIRE(restored->enums()[0].values[0].value == 0);
    REQUIRE(restored->enums()[0].values[0].comment == "resting");
}

TEST_CASE("an enum stored without a class round-trips", "[reclass]")
{
    Project project;
    auto*   color = project.add_enum("Color", 1);
    REQUIRE(color != nullptr);
    color->values.push_back(slopkit::reclass::EnumValue {.name = "Red", .value = 1, .comment = {}});

    const std::vector<Member> members = to_members(project);
    REQUIRE(members.size() == 2);
    REQUIRE(members[0].name == "reclass/index.txt");
    REQUIRE(members[0].text == "v2\n");
    REQUIRE(members[1].name == "reclass/enums.txt");
    REQUIRE(members[1].text == "enum name=\"Color\" size=1\nvalue name=\"Red\" value=1 comment=\"\"\n");

    const auto restored = from_members(members);
    REQUIRE(restored.has_value());
    REQUIRE(restored->classes().empty());
    require_same(project, *restored);
}

TEST_CASE("an empty project writes and reads no members", "[reclass]")
{
    const Project empty;

    REQUIRE(to_members(empty).empty());

    const auto restored = from_members(std::span<const Member> {});
    REQUIRE(restored.has_value());
    REQUIRE(restored->classes().empty());
}

TEST_CASE("class member names are sanitised and made unique", "[reclass]")
{
    Project project;
    project.add_class("A B", "app+0x10");
    project.add_class("A_B", "app+0x20");
    project.add_class("A?B", "app+0x30");

    const std::vector<Member> members = to_members(project);
    REQUIRE(members.size() == 4);
    REQUIRE(members[0].text
            == "v2\n"
               "reclass/classes/A_B.txt\n"
               "reclass/classes/A_B_2.txt\n"
               "reclass/classes/A_B_3.txt\n");
    REQUIRE(members[1].name == "reclass/classes/A_B.txt");
    REQUIRE(members[2].name == "reclass/classes/A_B_2.txt");
    REQUIRE(members[3].name == "reclass/classes/A_B_3.txt");

    const auto restored = from_members(members);
    REQUIRE(restored.has_value());
    require_same(project, *restored);
}

TEST_CASE("quotes, backslashes and newlines survive the member text", "[reclass]")
{
    Project project;
    project.add_class("Player \"one\"", "app+0x10 \\ path");

    auto& player = project.classes().front();
    player.nodes.push_back(node_of(project, NodeKind::int32, "a\nb"));
    player.nodes.back()->comment = "say \"hi\"";

    const auto restored = from_members(to_members(project));
    REQUIRE(restored.has_value());
    require_same(project, *restored);
}

TEST_CASE("a pre-v2 class member comes back as padding", "[reclass]")
{
    const auto project = from_members(std::vector<Member> {
        {         .name = "reclass/index.txt",.text = "reclass/classes/Player.txt\n"                                },
        {.name = "reclass/classes/Player.txt",
         .text = "name=\"Player\" address=\"game.exe+0x1234\"\n"
         "node kind=i32 name=\"health\" hex=0 comment=\"hit points\"\n"},
    });

    REQUIRE(project.has_value());
    REQUIRE(project->classes().size() == 1);
    CHECK(project->classes()[0].name == "Player");
    CHECK(project->classes()[0].address_expression == "game.exe+0x1234");
    // 1024 bytes of default padding, whatever the old payload held.
    CHECK(project->classes()[0].nodes.size() == 128);
}

TEST_CASE("a class member without an index is refused", "[reclass]")
{
    const std::vector<Member> members {
        {.name = "reclass/classes/Player.txt", .text = "v2\nname=\"Player\" address=\"1\"\n"},
    };

    const auto project = from_members(members);
    REQUIRE_FALSE(project.has_value());
    REQUIRE(project.error().find("reclass/index.txt") != std::string::npos);
}

TEST_CASE("an index that lists a missing class is refused", "[reclass]")
{
    const std::vector<Member> members {
        {.name = "reclass/index.txt", .text = "v2\nreclass/classes/Player.txt\n"},
    };

    const auto project = from_members(members);
    REQUIRE_FALSE(project.has_value());
    REQUIRE(project.error().find("missing class") != std::string::npos);
    REQUIRE(project.error().find("Player.txt") != std::string::npos);
}

TEST_CASE("a class member that is not listed is refused", "[reclass]")
{
    const std::vector<Member> members {
        {         .name = "reclass/index.txt",    .text = "v2\nreclass/classes/Player.txt\n"},
        {.name = "reclass/classes/Player.txt", .text = "v2\nname=\"Player\" address=\"1\"\n"},
        { .name = "reclass/classes/Enemy.txt",  .text = "v2\nname=\"Enemy\" address=\"2\"\n"},
    };

    const auto project = from_members(members);
    REQUIRE_FALSE(project.has_value());
    REQUIRE(project.error().find("Enemy.txt") != std::string::npos);
    REQUIRE(project.error().find("not listed") != std::string::npos);
}

TEST_CASE("a malformed class member is refused with its name", "[reclass]")
{
    const auto with_body = [](std::string body)
    {
        return std::vector<Member> {
            {         .name = "reclass/index.txt", .text = "v2\nreclass/classes/Player.txt\n"},
            {.name = "reclass/classes/Player.txt",           .text = "v2\n" + std::move(body)},
        };
    };

    const auto unknown_kind = from_members(
        with_body("name=\"Player\" address=\"1\"\nnode depth=0 kind=i128 flags=0 name=\"a\" comment=\"\"\n"));
    REQUIRE_FALSE(unknown_kind.has_value());
    REQUIRE(unknown_kind.error().find("Player.txt") != std::string::npos);
    REQUIRE(unknown_kind.error().find("unknown node kind") != std::string::npos);

    const auto missing_flags =
        from_members(with_body("name=\"Player\" address=\"1\"\nnode depth=0 kind=i32 name=\"a\" comment=\"\"\n"));
    REQUIRE_FALSE(missing_flags.has_value());
    REQUIRE(missing_flags.error().find("'flags'") != std::string::npos);

    const auto missing_depth =
        from_members(with_body("name=\"Player\" address=\"1\"\nnode kind=i32 flags=0 name=\"a\" comment=\"\"\n"));
    REQUIRE_FALSE(missing_depth.has_value());
    REQUIRE(missing_depth.error().find("'depth'") != std::string::npos);

    const auto unknown_key = from_members(with_body("name=\"Player\" address=\"1\" colour=\"red\"\n"));
    REQUIRE_FALSE(unknown_key.has_value());
    REQUIRE(unknown_key.error().find("unknown key") != std::string::npos);

    const auto missing_name = from_members(with_body("address=\"1\"\n"));
    REQUIRE_FALSE(missing_name.has_value());
    REQUIRE(missing_name.error().find("missing 'name'") != std::string::npos);

    const auto empty_name = from_members(with_body("name=\"\" address=\"1\"\n"));
    REQUIRE_FALSE(empty_name.has_value());
    REQUIRE(empty_name.error().find("empty class name") != std::string::npos);

    const auto stray_line = from_members(with_body("name=\"Player\" address=\"1\"\nnonsense\n"));
    REQUIRE_FALSE(stray_line.has_value());
    REQUIRE(stray_line.error().find("expected a node line") != std::string::npos);

    const auto bad_bitfield = from_members(
        with_body("name=\"Player\" address=\"1\"\nnode depth=0 kind=bitfield flags=0 name=\"a\" comment=\"\"\n"));
    REQUIRE_FALSE(bad_bitfield.has_value());
    REQUIRE(bad_bitfield.error().find("'bits'") != std::string::npos);

    const auto bad_depth = from_members(with_body("name=\"Player\" address=\"1\"\n"
                                                  "node depth=0 kind=i32 flags=0 name=\"a\" comment=\"\"\n"
                                                  "node depth=2 kind=i32 flags=0 name=\"b\" comment=\"\"\n"));
    REQUIRE_FALSE(bad_depth.has_value());
    REQUIRE(bad_depth.error().find("depth") != std::string::npos);
}

TEST_CASE("a malformed enums member is refused", "[reclass]")
{
    const auto with_enums = [](std::string enums)
    {
        return std::vector<Member> {
            {.name = "reclass/index.txt",           .text = "v2\n"},
            {.name = "reclass/enums.txt", .text = std::move(enums)},
        };
    };

    const auto missing_size = from_members(with_enums("enum name=\"State\"\n"));
    REQUIRE_FALSE(missing_size.has_value());
    REQUIRE(missing_size.error().find("size") != std::string::npos);

    const auto bad_size = from_members(with_enums("enum name=\"State\" size=3\n"));
    REQUIRE_FALSE(bad_size.has_value());
    REQUIRE(bad_size.error().find("size") != std::string::npos);

    const auto stray_value = from_members(with_enums("value name=\"Idle\" value=0\n"));
    REQUIRE_FALSE(stray_value.has_value());
    REQUIRE(stray_value.error().find("before any enum") != std::string::npos);

    const auto stray_line = from_members(with_enums("nonsense\n"));
    REQUIRE_FALSE(stray_line.has_value());
    REQUIRE(stray_line.error().find("expected an enum or value line") != std::string::npos);
}

TEST_CASE("an index with a duplicate or foreign entry is refused", "[reclass]")
{
    const auto duplicate = from_members(std::vector<Member> {
        {         .name = "reclass/index.txt", .text = "v2\nreclass/classes/Player.txt\nreclass/classes/Player.txt\n"},
        {.name = "reclass/classes/Player.txt",                          .text = "v2\nname=\"Player\" address=\"1\"\n"},
    });
    REQUIRE_FALSE(duplicate.has_value());
    REQUIRE(duplicate.error().find("duplicate entry") != std::string::npos);

    const auto foreign = from_members(std::vector<Member> {
        {.name = "reclass/index.txt", .text = "v2\nversion.txt\n"},
    });
    REQUIRE_FALSE(foreign.has_value());
    REQUIRE(foreign.error().find("unexpected entry") != std::string::npos);
}

TEST_CASE("two classes with the same name are refused", "[reclass]")
{
    const auto project = from_members(std::vector<Member> {
        {           .name = "reclass/index.txt", .text = "v2\nreclass/classes/Player.txt\nreclass/classes/Player_2.txt\n"},
        {  .name = "reclass/classes/Player.txt",                            .text = "v2\nname=\"Player\" address=\"1\"\n"},
        {.name = "reclass/classes/Player_2.txt",                            .text = "v2\nname=\"Player\" address=\"2\"\n"},
    });
    REQUIRE_FALSE(project.has_value());
    REQUIRE(project.error().find("duplicate class name") != std::string::npos);
}

TEST_CASE("an empty index restores an empty project", "[reclass]")
{
    const auto project = from_members(std::vector<Member> {
        {.name = "reclass/index.txt", .text = "v2\n"},
    });
    REQUIRE(project.has_value());
    REQUIRE(project->classes().empty());
}
