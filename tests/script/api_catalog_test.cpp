#include <catch2/catch.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "script/api_catalog.hpp"
#include "support/script_helpers.hpp"

namespace
{
    using slopkit::script::api_catalog;
    using slopkit::script::ApiEntry;
    using slopkit::script::find;
    using slopkit::script::lua_library_members;
    using slopkit::script::lua_standard_globals;
    using slopkit::script::matches;
    using slopkit::script::members_of;
    using slopkit::script::parameter_index;

    // Runs the engine's own `_G` and the members of the tables it binds through
    // a chunk, so the catalogue is checked against the live state rather than a
    // copy of itself.
    struct EngineNames
    {
        std::set<std::string> globals;
        std::set<std::string> members;
    };

    EngineNames engine_names(ScriptFixture& fixture)
    {
        constexpr std::string_view kChunk = R"lua(
local globals = {}
for key in pairs(_G) do
    globals[#globals + 1] = key
end
table.sort(globals)
print("G")
for _, key in ipairs(globals) do
    print(key)
end

local members = {}
for _, table_name in ipairs({"mem", "hook", "string", "table", "math"}) do
    local value = _G[table_name]
    if type(value) == "table" then
        for key in pairs(value) do
            members[#members + 1] = table_name .. "." .. key
        end
    end
end
table.sort(members)
print("M")
for _, key in ipairs(members) do
    print(key)
end
)lua";

        const RunResult result = fixture.engine.run(kChunk);
        REQUIRE(result.ok);
        REQUIRE(result.error.empty());

        EngineNames names;
        bool        in_members = false;
        for (const std::string& line : result.output)
        {
            if (line == "G")
            {
                continue;
            }
            if (line == "M")
            {
                in_members = true;
                continue;
            }
            (in_members ? names.members : names.globals).insert(line);
        }
        return names;
    }

    std::set<std::string> catalogue_globals()
    {
        std::set<std::string> names;
        for (const ApiEntry& entry : api_catalog())
        {
            if (entry.name.find('.') == std::string_view::npos)
            {
                names.emplace(entry.name);
            }
        }
        return names;
    }

    std::set<std::string> catalogue_members()
    {
        std::set<std::string> names;
        for (const ApiEntry& entry : api_catalog())
        {
            if (entry.name.find('.') != std::string_view::npos)
            {
                names.emplace(entry.name);
            }
        }
        return names;
    }

    std::set<std::string> to_set(std::span<const std::string_view> values)
    {
        std::set<std::string> names;
        for (const std::string_view value : values)
        {
            names.emplace(value);
        }
        return names;
    }

    // sol2 records the libraries it opened and its own trampoline and protected
    // handlers as private keys in `_G` ("base", "sol.☢☢", "sol.🔩"); a script
    // never sees or uses them, so the guard ignores them.
    bool is_sol_internal(std::string_view name)
    {
        return name == "base" || name.starts_with("sol.");
    }
} // namespace

TEST_CASE("the API catalogue matches the globals a live engine binds", "[script]")
{
    ScriptFixture     fixture;
    const EngineNames names = engine_names(fixture);

    // What the engine binds is its `_G` minus the Lua standard library it opens.
    std::set<std::string> engine_globals;
    const auto            standard = to_set(lua_standard_globals());
    std::ranges::set_difference(names.globals, standard, std::inserter(engine_globals, engine_globals.end()));
    std::erase_if(engine_globals,
                  [](const std::string& name)
                  {
                      return is_sol_internal(name);
                  });

    std::set<std::string> catalogued;
    std::ranges::set_difference(catalogue_globals(), standard, std::inserter(catalogued, catalogued.end()));

    // The two sets are equal: no engine global is missing from the catalogue and
    // no catalogue global names something the engine does not bind.
    CHECK(engine_globals == catalogued);
    for (const std::string& name : engine_globals)
    {
        INFO("engine global not in the catalogue: " << name);
        CHECK(find(name) != nullptr);
    }

    // Every standard-library name is a real global, so the popup never offers a
    // name the engine cannot resolve.
    for (const std::string_view name : lua_standard_globals())
    {
        INFO("lua standard global the engine does not bind: " << name);
        CHECK(names.globals.contains(std::string {name}));
    }

    // The table members: `mem` and `hook` come from the catalogue, `string`,
    // `table` and `math` from the curated library list.
    std::set<std::string> engine_members = names.members;
    for (const std::string_view member : lua_library_members())
    {
        INFO("lua library member the engine does not bind: " << member);
        CHECK(engine_members.contains(std::string {member}));
    }
    std::set<std::string> catalogue_only;
    const auto            library = to_set(lua_library_members());
    std::ranges::set_difference(engine_members, library, std::inserter(catalogue_only, catalogue_only.end()));
    CHECK(catalogue_only == catalogue_members());
}

TEST_CASE("every API entry is complete and unique", "[script]")
{
    std::set<std::string> names;
    for (const ApiEntry& entry : api_catalog())
    {
        INFO("entry: " << entry.name);
        CHECK_FALSE(entry.name.empty());
        // The name opens its own signature, so the popup always shows the name
        // it completes.
        CHECK(entry.signature.starts_with(entry.name));
        CHECK_FALSE(entry.summary.empty());
        CHECK(names.insert(std::string {entry.name}).second);
    }

    CHECK(find("aobscan") != nullptr);
    CHECK(find("mem.read") != nullptr);
    CHECK(find("not_a_name") == nullptr);

    const std::vector<const ApiEntry*> mem = members_of("mem");
    CHECK(mem.size() == 6);
    for (const ApiEntry* entry : mem)
    {
        CHECK(entry->name.starts_with("mem."));
    }
}

TEST_CASE("catalogue matching filters by receiver and prefix", "[script]")
{
    // An empty receiver lists the globals; a receiver lists its members.
    const std::vector<const ApiEntry*> globals = matches({}, {});
    for (const ApiEntry* entry : globals)
    {
        CHECK(entry->name.find('.') == std::string_view::npos);
    }
    CHECK(std::ranges::is_sorted(globals, {}, &ApiEntry::name));

    const std::vector<const ApiEntry*> reads = matches("mem", "re");
    std::vector<std::string>           read_names;
    for (const ApiEntry* entry : reads)
    {
        read_names.emplace_back(entry->name);
    }
    CHECK(read_names == std::vector<std::string> {"mem.read", "mem.read_bytes", "mem.read_string"});

    // Case-insensitive, and an empty prefix lists the whole table.
    CHECK(matches("mem", "RE").size() == reads.size());
    CHECK(matches("mem", {}).size() == 6);
    CHECK(matches({}, "aobscan").size() == 1);
    CHECK(matches({}, "zzz").empty());
}

TEST_CASE("parameter_index emphasises a parameter inside the signature", "[script]")
{
    CHECK(parameter_index("mem.read(address, token)", 0) == 0);
    CHECK(parameter_index("mem.read(address, token)", 1) == 1);
    CHECK(parameter_index("mem.read(address, token)", 5) == 1); // clamped to the last
    CHECK(parameter_index("read_u8(address)", 0) == 0);
    CHECK(parameter_index("print()", 0) == 0);
    CHECK(parameter_index("mem", 0) == 0);
}
