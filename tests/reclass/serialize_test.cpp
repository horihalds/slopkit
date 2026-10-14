#include <catch2/catch.hpp>

#include <cstddef>
#include <initializer_list>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "reclass/node.hpp"
#include "reclass/project.hpp"
#include "reclass/serialize.hpp"
#include "support/reclass_helpers.hpp"

namespace
{
    using slopkit::reclass::from_members;
    using slopkit::reclass::kMemberPrefix;
    using slopkit::reclass::Member;
    using slopkit::reclass::NodeKind;
    using slopkit::reclass::Project;
    using slopkit::reclass::to_members;
    using slopkit::test::node_of;

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
                REQUIRE(actual.nodes[position]->kind() == expected.nodes[position]->kind());
                REQUIRE(actual.nodes[position]->name == expected.nodes[position]->name);
                REQUIRE(actual.nodes[position]->comment == expected.nodes[position]->comment);
                REQUIRE(actual.nodes[position]->class_name == expected.nodes[position]->class_name);
                REQUIRE(actual.nodes[position]->hex == expected.nodes[position]->hex);
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
    REQUIRE(members[0].text == "reclass/classes/Player.txt\n");
    REQUIRE(members[1].name == "reclass/classes/Player.txt");
    REQUIRE(members[1].text
            == "name=\"Player\" address=\"game.exe+0x1234\"\n"
               "node kind=i32 name=\"health\" hex=0 comment=\"\"\n");
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
            == "reclass/classes/A_B.txt\n"
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

TEST_CASE("a class member without an index is refused", "[reclass]")
{
    const std::vector<Member> members {
        {.name = "reclass/classes/Player.txt", .text = "name=\"Player\" address=\"1\"\n"},
    };

    const auto project = from_members(members);
    REQUIRE_FALSE(project.has_value());
    REQUIRE(project.error().find("reclass/index.txt") != std::string::npos);
}

TEST_CASE("an index that lists a missing class is refused", "[reclass]")
{
    const std::vector<Member> members {
        {.name = "reclass/index.txt", .text = "reclass/classes/Player.txt\n"},
    };

    const auto project = from_members(members);
    REQUIRE_FALSE(project.has_value());
    REQUIRE(project.error().find("missing class") != std::string::npos);
    REQUIRE(project.error().find("Player.txt") != std::string::npos);
}

TEST_CASE("a class member that is not listed is refused", "[reclass]")
{
    const std::vector<Member> members {
        {         .name = "reclass/index.txt",    .text = "reclass/classes/Player.txt\n"},
        {.name = "reclass/classes/Player.txt", .text = "name=\"Player\" address=\"1\"\n"},
        { .name = "reclass/classes/Enemy.txt",  .text = "name=\"Enemy\" address=\"2\"\n"},
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
            {         .name = "reclass/index.txt", .text = "reclass/classes/Player.txt\n"},
            {.name = "reclass/classes/Player.txt",                .text = std::move(body)},
        };
    };

    const auto unknown_kind =
        from_members(with_body("name=\"Player\" address=\"1\"\nnode kind=i128 name=\"a\" hex=0 comment=\"\"\n"));
    REQUIRE_FALSE(unknown_kind.has_value());
    REQUIRE(unknown_kind.error().find("Player.txt") != std::string::npos);
    REQUIRE(unknown_kind.error().find("unknown node kind") != std::string::npos);

    const auto missing_hex =
        from_members(with_body("name=\"Player\" address=\"1\"\nnode kind=i32 name=\"a\" comment=\"\"\n"));
    REQUIRE_FALSE(missing_hex.has_value());
    REQUIRE(missing_hex.error().find("'hex'") != std::string::npos);

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
}

TEST_CASE("an index with a duplicate or foreign entry is refused", "[reclass]")
{
    const auto duplicate = from_members(std::vector<Member> {
        {         .name = "reclass/index.txt", .text = "reclass/classes/Player.txt\nreclass/classes/Player.txt\n"},
        {.name = "reclass/classes/Player.txt",                          .text = "name=\"Player\" address=\"1\"\n"},
    });
    REQUIRE_FALSE(duplicate.has_value());
    REQUIRE(duplicate.error().find("duplicate entry") != std::string::npos);

    const auto foreign = from_members(std::vector<Member> {
        {.name = "reclass/index.txt", .text = "version.txt\n"},
    });
    REQUIRE_FALSE(foreign.has_value());
    REQUIRE(foreign.error().find("unexpected entry") != std::string::npos);
}

TEST_CASE("two classes with the same name are refused", "[reclass]")
{
    const auto project = from_members(std::vector<Member> {
        {           .name = "reclass/index.txt", .text = "reclass/classes/Player.txt\nreclass/classes/Player_2.txt\n"},
        {  .name = "reclass/classes/Player.txt",                            .text = "name=\"Player\" address=\"1\"\n"},
        {.name = "reclass/classes/Player_2.txt",                            .text = "name=\"Player\" address=\"2\"\n"},
    });
    REQUIRE_FALSE(project.has_value());
    REQUIRE(project.error().find("duplicate class name") != std::string::npos);
}

TEST_CASE("an empty index restores an empty project", "[reclass]")
{
    const auto project = from_members(std::vector<Member> {
        {.name = "reclass/index.txt", .text = "\n"},
    });
    REQUIRE(project.has_value());
    REQUIRE(project->classes().empty());
}
