#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "reclass/node.hpp"
#include "reclass/project.hpp"
#include "script/engine.hpp"
#include "script/structs_view.hpp"
#include "script/types.hpp"
#include "support/reclass_helpers.hpp"
#include "support/script_helpers.hpp"

namespace
{
    using slopkit::reclass::NodeKind;
    using slopkit::reclass::Project;
    using slopkit::script::class_index_of;
    using slopkit::script::RunResult;

    // The player instance the fixture's offsets describe sits at the fake
    // target's base; `point` is the address a class pointer field names.
    constexpr std::uint64_t kPlayerBase = 0x1000;
    constexpr std::uint64_t kWeaponBase = 0x1020;
    constexpr std::uint64_t kTargets    = 0x10A0;
    constexpr std::uint64_t kNext       = 0x1080;

    struct PlayerFixture
    {
        Project       project;
        ScriptFixture script;

        PlayerFixture() : script(0x100)
        {
            build_structs_fixture(project);
            script.engine.set_classes(class_index_of(project));
        }

        [[nodiscard]] RunResult run(std::string_view chunk)
        {
            return script.engine.run(chunk);
        }

        [[nodiscard]] bool failed(RunResult result, std::string_view needle)
        {
            INFO("script error: " << result.error);
            return !result.ok && result.error.find(needle) != std::string::npos;
        }
    };

    void require_ok(const RunResult& result)
    {
        INFO("script error: " << result.error);
        REQUIRE(result.ok);
    }
}

TEST_CASE("structs names the table's classes", "[script][structs]")
{
    PlayerFixture fixture;

    const RunResult listed = fixture.run(R"(
print(#structs.list())
print(structs.list()[1])
print(structs.list()[3])
print(tostring(structs.get("Player")))
print(tostring(structs.get("Weapon")))
)");

    require_ok(listed);
    REQUIRE(listed.output.size() == 5);
    CHECK(listed.output[0] == "3");
    CHECK(listed.output[1] == "Weapon");
    CHECK(listed.output[2] == "Player");
    CHECK(listed.output[3] == "Player");
    CHECK(listed.output[4] == "Weapon");
}

TEST_CASE("structs reports a class it does not have", "[script][structs]")
{
    ScriptFixture bare {0x100};

    CHECK(bare.engine.run(R"(print(#structs.list()))").output.at(0) == "0");

    const RunResult missing = bare.engine.run(R"(print(structs.get("Player")))");
    REQUIRE_FALSE(missing.ok);
    CHECK(missing.error.find("structs.get: no class named 'Player'") != std::string::npos);

    const RunResult not_a_name = bare.engine.run(R"(print(structs.get(5)))");
    REQUIRE_FALSE(not_a_name.ok);
    CHECK(not_a_name.error.find("structs.get: the class name must be a string") != std::string::npos);
}

TEST_CASE("a class view validates the address it binds", "[script][structs]")
{
    PlayerFixture fixture;

    for (const std::string_view chunk : {std::string_view {R"(print(structs.get("Player"):at(-1)))"},
                                         std::string_view {R"(print(structs.get("Player"):at(1.5)))"},
                                         std::string_view {R"(print(structs.get("Player"):at("0x1000")))"},
                                         std::string_view {R"(print(structs.get("Player"):at()))"}})
    {
        INFO("chunk: " << chunk);
        const RunResult result = fixture.run(chunk);
        REQUIRE_FALSE(result.ok);
        CHECK(result.error.find("Player:at: the address must be a whole non-negative number") != std::string::npos);
    }

    CHECK(fixture.run(R"(print(tostring(structs.get("Player"):at(0x1000))))").output.at(0) == "Player@0x1000");
}

TEST_CASE("an instance reads and writes the fields of its class", "[script][structs]")
{
    PlayerFixture fixture;
    fixture.script.fake.put_value<float>(0, 12.5F);
    fixture.script.fake.put_value<std::int32_t>(4, 7);
    fixture.script.fake.put_value<std::uint8_t>(44, 1);
    fixture.script.fake.put_text(36, "hero");
    fixture.script.fake.put_value<double>(77, 2.5);

    const RunResult result = fixture.run(R"(
local p = structs.get("Player"):at(0x1000)
print(p.health)
print(p["ammo"])
print(p.alive)
print(p.name)
print(p.flags)
print(p.speed)
print(#p.blob)
p.health = 100
p.ammo = 30
p.alive = false
p.name = "villain"
print(p.health)
print(p.ammo)
print(p.alive)
print(p.name)
print(p:base())
print(p:address_of("ammo"))
print(p:address_of("speed"))
)");

    require_ok(result);
    REQUIRE(result.output.size() == 14);
    CHECK(result.output[0] == "12.5");
    CHECK(result.output[1] == "7");
    CHECK(result.output[2] == "true");
    CHECK(result.output[3] == "hero");
    CHECK(result.output[4] == "0");
    CHECK(result.output[5] == "2.5");
    CHECK(result.output[6] == "12");
    CHECK(result.output[7] == "100.0");
    CHECK(result.output[8] == "30");
    CHECK(result.output[9] == "false");
    CHECK(result.output[10] == "villain");
    CHECK(result.output[11] == "4096");
    CHECK(result.output[12] == "4100");
    CHECK(result.output[13] == "4173");

    CHECK(fixture.script.fake.take(0, 4)
          == std::vector<std::byte> {std::byte {0x00}, std::byte {0x00}, std::byte {0xC8}, std::byte {0x42}});
    CHECK(fixture.script.fake.take(4, 4)
          == std::vector<std::byte> {std::byte {0x1E}, std::byte {0x00}, std::byte {0x00}, std::byte {0x00}});
}

TEST_CASE("a bad field access names the class and the field", "[script][structs]")
{
    PlayerFixture fixture;
    fixture.script.fake.put_value<std::int32_t>(45, 0xDEADBEEFU);

    CHECK(fixture.failed(fixture.run(R"(print(structs.get("Player"):at(0x1000).nope))"), "Player.nope: no such field"));
    CHECK(fixture.failed(fixture.run(R"(structs.get("Player"):at(0x1000).nope = 1)"), "Player.nope: no such field"));
    CHECK(fixture.failed(fixture.run(R"(structs.get("Player"):at(0x1000).ammo = 1.5)"),
                         "Player.ammo: the field holds a whole number, so 1.5 is refused"));
    CHECK(fixture.failed(fixture.run(R"(structs.get("Player"):at(0x1000).ammo = "seven")"),
                         "Player.ammo: the value must be a number"));
    CHECK(fixture.failed(fixture.run(R"(structs.get("Player"):at(0x1000).weapon = true)"),
                         "Player.weapon: the value must be an address"));
    CHECK(fixture.failed(fixture.run(R"(structs.get("Player"):at(0x1000).pos = 1)"),
                         "Player.pos: an inlined class cannot be written"));
    CHECK(fixture.failed(fixture.run(R"(structs.get("Player"):at(0x1000).items = 1)"),
                         "Player.items: an array cannot be written"));
    CHECK(fixture.failed(fixture.run(R"(print(structs.get("Player"):at(0x1000).u))"),
                         "Player.u: the 'Union' kind has no script value"));
    CHECK(fixture.failed(fixture.run(R"(print(structs.get("Player"):at(0x9000).health))"),
                         "Player.health: address is not mapped"));
    CHECK(fixture.failed(fixture.run(R"(structs.get("Player"):at(0x1000).flags = 4294967296)"),
                         "Player.flags: the value is outside the field's 'u32' range"));
    CHECK(fixture.failed(fixture.run(R"(structs.get("Player"):at(0x1000).name = "far too long for this field")"),
                         "Player.name: the text does not fit in 8 character(s)"));
}

TEST_CASE("a class pointer follows one dereference per access", "[script][structs]")
{
    PlayerFixture fixture;
    fixture.script.fake.put_value<float>(0, 12.5F);
    fixture.script.fake.put_value<std::uint64_t>(8, kWeaponBase);
    fixture.script.fake.put_value<std::uint64_t>(49, kNext);
    fixture.script.fake.put_value<float>(static_cast<std::size_t>(kWeaponBase - kPlayerBase), 9.5F);

    const RunResult result = fixture.run(R"(
local p = structs.get("Player"):at(0x1000)
print(p.weapon.damage)
print(p.weapon:base())
print(p:raw("weapon"))
print(p.next)
print(p.weapon.damage == structs.get("Weapon"):at(0x1020).damage)
p.weapon.damage = 3.25
print(p.weapon.damage)
print(read_f32(0x1020))
p.next = p.weapon
print(read_u64(0x1031))
p.weapon = nil
print(read_u64(0x1008))
)");

    require_ok(result);
    REQUIRE(result.output.size() == 9);
    CHECK(result.output[0] == "9.5");
    CHECK(result.output[1] == "4128");
    CHECK(result.output[2] == "4128");
    CHECK(result.output[3] == "4224");
    CHECK(result.output[4] == "true");
    CHECK(result.output[5] == "3.25");
    CHECK(result.output[6] == "3.25");
    CHECK(result.output[7] == "4128");
    CHECK(result.output[8] == "0");
}

TEST_CASE("a null class pointer reads as nothing", "[script][structs]")
{
    PlayerFixture fixture;

    const RunResult result = fixture.run(R"(
local p = structs.get("Player"):at(0x1000)
if p.weapon == nil then print("nil") end
print(p:raw("weapon"))
)");

    require_ok(result);
    REQUIRE(result.output.size() == 2);
    CHECK(result.output[0] == "nil");
    CHECK(result.output[1] == "0");
}

TEST_CASE("a renamed or deleted class reaches the next access", "[script][structs]")
{
    PlayerFixture fixture;
    fixture.script.fake.put_value<float>(0, 12.5F);
    fixture.script.fake.put_value<std::uint64_t>(8, kWeaponBase);

    REQUIRE(fixture.run(R"(held = structs.get("Player"):at(0x1000) print(held.health))").output.at(0) == "12.5");

    fixture.project.classes().back().nodes.front()->name = "hp";
    fixture.script.engine.set_classes(class_index_of(fixture.project));

    CHECK(fixture.failed(fixture.run(R"(print(held.health))"), "Player.health: no such field"));
    CHECK(fixture.run(R"(print(held.hp))").output.at(0) == "12.5");
    CHECK(fixture.run(R"(print(structs.get("Player"):at(0x1000).hp))").output.at(0) == "12.5");

    fixture.project.remove_class(2);
    fixture.script.engine.set_classes(class_index_of(fixture.project));

    CHECK(fixture.failed(fixture.run(R"(print(held.hp))"), "Player: no class named 'Player'"));
    CHECK(fixture.failed(fixture.run(R"(print(structs.get("Player")))"), "structs.get: no class named 'Player'"));
    CHECK(fixture.run(R"(print(tostring(structs.get("Weapon"))))").output.at(0) == "Weapon");
    CHECK(fixture.run(R"(print(tostring(structs.get("Vec"))))").output.at(0) == "Vec");
}

TEST_CASE("a class pointer to a deleted class names it", "[script][structs]")
{
    PlayerFixture fixture;
    fixture.script.fake.put_value<std::uint64_t>(8, kWeaponBase);

    fixture.project.remove_class(0);
    fixture.script.engine.set_classes(class_index_of(fixture.project));

    CHECK(fixture.failed(fixture.run(R"(print(structs.get("Player"):at(0x1000).weapon))"),
                         "Player.weapon: no class named 'Weapon'"));
}

TEST_CASE("a script-held instance fails cleanly once its classes are gone", "[script][structs]")
{
    PlayerFixture fixture;
    fixture.script.fake.put_value<float>(0, 1.0F);

    REQUIRE(fixture.run(R"(held = structs.get("Player"):at(0x1000) print(held.health))").output.at(0) == "1.0");

    fixture.script.engine.set_classes(nullptr);

    CHECK(fixture.failed(fixture.run(R"(print(held.health))"), "Player: no class named 'Player'"));
    CHECK(fixture.failed(fixture.run(R"(print(structs.get("Player")))"), "structs.get: no class named 'Player'"));
    CHECK(fixture.run(R"(print(#structs.list()))").output.at(0) == "0");
}

TEST_CASE("class lookups work with no target attached", "[script][structs]")
{
    slopkit::script::SymbolTable symbols;
    slopkit::script::Engine      engine {slopkit::script::MemoryApi {}, symbols.api()};

    Project project;
    build_structs_fixture(project);
    engine.set_classes(class_index_of(project));

    CHECK(engine.run(R"(print(#structs.list()))").output.at(0) == "3");

    const RunResult read = engine.run(R"(print(structs.get("Player"):at(0x1000).health))");
    REQUIRE_FALSE(read.ok);
    CHECK(read.error.find("Player.health: no target is attached") != std::string::npos);
}

TEST_CASE("array proxies index from one and refuse what is out of range", "[script][structs]")
{
    PlayerFixture fixture;
    fixture.script.fake.put_value<std::int32_t>(24, 11);
    fixture.script.fake.put_value<std::int32_t>(28, 22);
    fixture.script.fake.put_value<std::int32_t>(32, 33);
    fixture.script.fake.put_value<float>(61, 5.0F);
    fixture.script.fake.put_value<float>(65, 6.0F);
    fixture.script.fake.put_value<std::uint64_t>(69, kTargets);
    fixture.script.fake.put_value<float>(static_cast<std::size_t>(kTargets - kPlayerBase), 1.0F);
    fixture.script.fake.put_value<float>(static_cast<std::size_t>(kTargets - kPlayerBase) + 4, 2.0F);

    const RunResult result = fixture.run(R"(
local p = structs.get("Player"):at(0x1000)
print(p.items[1])
print(p.items[3])
print(#p.items)
print(tostring(p.items))
print(p.slots[2].damage)
print(p.targets[1].damage)
print(p.targets[2].damage)
p.items[2] = 44
print(read_i32(0x101C))
)");

    require_ok(result);
    REQUIRE(result.output.size() == 8);
    CHECK(result.output[0] == "11");
    CHECK(result.output[1] == "33");
    CHECK(result.output[2] == "3");
    CHECK(result.output[3] == "Player.items[3]@0x1018");
    CHECK(result.output[4] == "6.0");
    CHECK(result.output[5] == "1.0");
    CHECK(result.output[6] == "2.0");
    CHECK(result.output[7] == "44");

    CHECK(fixture.failed(fixture.run(R"(print(structs.get("Player"):at(0x1000).items[4]))"),
                         "Player.items[4]: the index is out of range (the array holds 3 element(s))"));
    CHECK(fixture.failed(fixture.run(R"(structs.get("Player"):at(0x1000).items[0] = 1)"),
                         "Player.items[0]: the index is out of range (the array holds 3 element(s))"));
    CHECK(fixture.failed(fixture.run(R"(structs.get("Player"):at(0x1000).items["one"] = 1)"),
                         "Player.items: the index must be a whole number"));
    CHECK(fixture.failed(fixture.run(R"(print(structs.get("Player"):at(0x1000).targets[3].damage))"),
                         "Player.targets[3]: the index is out of range (the array holds 2 element(s))"));
}

TEST_CASE("a snapshot serves its bytes and follows pointers live", "[script][structs]")
{
    PlayerFixture fixture;
    fixture.script.fake.put_value<float>(0, 12.5F);
    fixture.script.fake.put_value<std::uint64_t>(8, kWeaponBase);
    fixture.script.fake.put_value<float>(static_cast<std::size_t>(kWeaponBase - kPlayerBase), 9.5F);
    fixture.script.fake.put_value<std::int32_t>(24, 11);

    const RunResult result = fixture.run(R"(
local p = structs.get("Player"):at(0x1000)
local snap = p:snapshot()
write(0x1000, 50.0, "f32")
write(0x1020, 4.5, "f32")
write(0x1018, 77, "i32")
print(snap.health)
print(p.health)
print(snap.weapon.damage)
print(snap.items[1])
print(snap.pos.x)
print(tostring(snap))
print(snap:base())
)");

    require_ok(result);
    REQUIRE(result.output.size() == 7);
    CHECK(result.output[0] == "12.5");
    CHECK(result.output[1] == "50.0");
    CHECK(result.output[2] == "4.5");
    CHECK(result.output[3] == "11");
    CHECK(result.output[4] == "0.0");
    CHECK(result.output[5] == "Player@0x1000");
    CHECK(result.output[6] == "4096");

    CHECK(fixture.failed(fixture.run(R"(local snap = structs.get("Player"):at(0x1000):snapshot() snap.health = 1)"),
                         "Player.health: a snapshot is read-only"));
}

TEST_CASE("pairs, fields and the field helper reach every field", "[script][structs]")
{
    PlayerFixture fixture;
    fixture.script.fake.put_value<std::int32_t>(4, 7);

    const RunResult result = fixture.run(R"(
local p = structs.get("Player"):at(0x1000)
local names = {}
for name, value in pairs(p) do
    names[#names + 1] = name
    if name == "ammo" then print(value) end
end
table.sort(names)
print(table.concat(names, ","))

local fields = structs.get("Player"):fields()
print(#fields)
print(fields[1].name, fields[1].offset, fields[1].type, fields[1].size)
print(fields[5].name, fields[5].offset, fields[5].type, fields[5].size)
print(p:field("ammo"))
p:field("ammo", 21)
print(p.ammo)
for _, value in pairs(p.items) do print(value) end
)");

    require_ok(result);
    REQUIRE(result.output.size() == 10);
    CHECK(result.output[0] == "7");
    CHECK(result.output[1] == "alive,ammo,blob,flags,health,items,name,next,pos,slots,speed,tag,targets,u,weapon");
    CHECK(result.output[2] == "15");
    CHECK(result.output[3] == "health\t0\tf32\t4");
    CHECK(result.output[4] == "items\t24\ti32 [3]\t12");
    CHECK(result.output[5] == "7");
    CHECK(result.output[6] == "21");
    CHECK(result.output[7] == "0");
    CHECK(result.output[8] == "0");
    CHECK(result.output[9] == "0");
}

TEST_CASE("a field named like a helper stays reachable through field()", "[script][structs]")
{
    Project project;
    project.add_class("Odd", "app");
    auto& type = project.classes().back();
    type.nodes.push_back(slopkit::test::node_of(project, NodeKind::int32, "base"));
    type.nodes.push_back(slopkit::test::node_of(project, NodeKind::int32, "raw"));

    ScriptFixture script {0x100};
    script.engine.set_classes(class_index_of(project));
    script.fake.put_value<std::int32_t>(0, 5);
    script.fake.put_value<std::int32_t>(4, 6);

    const RunResult result = script.engine.run(R"(
local odd = structs.get("Odd"):at(0x1000)
print(type(odd.base))
print(type(odd.raw))
print(odd:base())
print(odd:field("base"))
print(odd:field("raw"))
odd:field("base", 9)
print(odd:field("base"))
)");

    require_ok(result);
    REQUIRE(result.output.size() == 6);
    CHECK(result.output[0] == "function");
    CHECK(result.output[1] == "function");
    CHECK(result.output[2] == "4096");
    CHECK(result.output[3] == "5");
    CHECK(result.output[4] == "6");
    CHECK(result.output[5] == "9");
    CHECK(script.fake.take(0, 4)
          == std::vector<std::byte> {std::byte {0x09}, std::byte {0x00}, std::byte {0x00}, std::byte {0x00}});

    const RunResult refused = script.engine.run(R"(structs.get("Odd"):at(0x1000).base = 1)");
    REQUIRE_FALSE(refused.ok);
    CHECK(refused.error.find("Odd.base: 'base' is a helper; write the field with field('base', value)")
          != std::string::npos);
}
