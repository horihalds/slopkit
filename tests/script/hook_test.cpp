#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "script/hook.hpp"
#include "support/script_helpers.hpp"

namespace
{
    using namespace slopkit;

    // A window of eight whole bytes and a one-byte stub, so the payload layout
    // and the rounded-up cave size are easy to predict.
    script::hook::Spec simple_spec()
    {
        script::hook::Spec spec;
        spec.site_name       = "hook_site_10";
        spec.cave_name       = "hook_cave_10";
        spec.original        = {std::byte {0x48},
                                std::byte {0x83},
                                std::byte {0xEC},
                                std::byte {0x28},
                                std::byte {0x48},
                                std::byte {0x89},
                                std::byte {0xD8},
                                std::byte {0x48}};
        spec.code_text       = "nop";
        spec.trampoline_text = "SUB RSP, 0x28";
        return spec;
    }

    // The bytes the fake target is primed with: the window plus NOP filler.
    void prime_site(FakeMemory& fake)
    {
        fake.put(0x10,
                 {0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xD8, 0x48, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90});
    }
} // namespace

TEST_CASE("hook::install writes the cave payload and the padded site in two writes", "[script][hook]")
{
    ScriptFixture            fixture(0x200);
    const script::hook::Spec hook_spec = simple_spec();
    prime_site(fixture.fake);

    MemoryApi           memory = fixture.fake.api();
    const std::uint64_t site   = fixture.fake.address(0x10);

    const std::expected<std::uint64_t, std::string> installed = script::hook::install(hook_spec, memory, site);
    REQUIRE(installed.has_value());
    const std::uint64_t cave = *installed;

    // The whole activation costs two target writes.
    CHECK(fixture.fake.write_count == 2);

    // Exactly one cave, sized from the payload: 1 + 4 + 5 rounded up.
    REQUIRE(fixture.fake.allocations.size() == 1);
    REQUIRE(fixture.fake.allocations.count(cave) == 1);
    CHECK(fixture.fake.allocations.at(cave) == 16);
    REQUIRE(fixture.fake.allocate_requests.size() == 1);
    CHECK(fixture.fake.allocate_requests.front().first == site);

    // The site holds a near jump and NOP padding.
    const auto site_offset = static_cast<std::size_t>(site - kScriptBase);
    CHECK(fixture.fake.bytes[site_offset] == std::byte {0xE9});
    for (std::size_t offset = 5; offset < hook_spec.original.size(); ++offset)
    {
        CHECK(fixture.fake.bytes[site_offset + offset] == std::byte {0x90});
    }

    // The cave holds the stub, then the trampoline, then the jump back.
    const auto cave_offset = static_cast<std::size_t>(cave - kScriptBase);
    CHECK(fixture.fake.bytes[cave_offset] == std::byte {0x90});     // nop
    CHECK(fixture.fake.bytes[cave_offset + 1] == std::byte {0x48}); // SUB RSP, 0x28
    CHECK(fixture.fake.bytes[cave_offset + 2] == std::byte {0x83});
    CHECK(fixture.fake.bytes[cave_offset + 3] == std::byte {0xEC});
    CHECK(fixture.fake.bytes[cave_offset + 4] == std::byte {0x28});
    CHECK(fixture.fake.bytes[cave_offset + 5] == std::byte {0xE9}); // the jump back
}

TEST_CASE("hook::remove restores the site and frees the cave", "[script][hook]")
{
    ScriptFixture            fixture(0x200);
    const script::hook::Spec hook_spec = simple_spec();
    prime_site(fixture.fake);

    MemoryApi           memory = fixture.fake.api();
    const std::uint64_t site   = fixture.fake.address(0x10);
    const auto          cave   = script::hook::install(hook_spec, memory, site);
    REQUIRE(cave.has_value());

    const std::expected<void, std::string> removed = script::hook::remove(hook_spec, memory, site, *cave);
    REQUIRE(removed.has_value());

    const auto site_offset = static_cast<std::size_t>(site - kScriptBase);
    for (std::size_t offset = 0; offset < hook_spec.original.size(); ++offset)
    {
        CHECK(fixture.fake.bytes[site_offset + offset] == hook_spec.original[offset]);
    }
    CHECK(fixture.fake.allocations.empty());
}

TEST_CASE("hook::install rounds the payload up and honours a cave_size override", "[script][hook]")
{
    ScriptFixture      fixture(0x200);
    script::hook::Spec hook_spec = simple_spec();
    hook_spec.cave_size          = 64;
    prime_site(fixture.fake);

    MemoryApi           memory = fixture.fake.api();
    const std::uint64_t site   = fixture.fake.address(0x10);
    const auto          cave   = script::hook::install(hook_spec, memory, site);
    REQUIRE(cave.has_value());
    REQUIRE(fixture.fake.allocations.count(*cave) == 1);
    CHECK(fixture.fake.allocations.at(*cave) == 64);
}

TEST_CASE("hook::install expands the trampoline's site-relative placeholders", "[script][hook]")
{
    ScriptFixture      fixture(0x200);
    script::hook::Spec hook_spec = simple_spec();
    hook_spec.trampoline_text    = "MOV EAX, 0x%X";
    hook_spec.address_args       = {0x2000};
    prime_site(fixture.fake);

    MemoryApi           memory = fixture.fake.api();
    const std::uint64_t site   = fixture.fake.address(0x10);
    const auto          cave   = script::hook::install(hook_spec, memory, site);
    REQUIRE(cave.has_value());

    // site + 0x2000 = 0x3010, encoded as `MOV EAX, 0x3010` (B8 10 30 00 00).
    const auto cave_offset = static_cast<std::size_t>(*cave - kScriptBase);
    CHECK(fixture.fake.bytes[cave_offset] == std::byte {0x90});
    CHECK(fixture.fake.bytes[cave_offset + 1] == std::byte {0xB8});
    CHECK(fixture.fake.bytes[cave_offset + 2] == std::byte {0x10});
    CHECK(fixture.fake.bytes[cave_offset + 3] == std::byte {0x30});
    CHECK(fixture.fake.bytes[cave_offset + 4] == std::byte {0x00});
    CHECK(fixture.fake.bytes[cave_offset + 5] == std::byte {0x00});
}

TEST_CASE("hook::install refuses bytes that are not the recorded instruction", "[script][hook]")
{
    ScriptFixture            fixture(0x200);
    const script::hook::Spec hook_spec = simple_spec();
    // A different first byte at the site.
    fixture.fake.put(0x10, {0x00, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xD8, 0x48});

    MemoryApi           memory = fixture.fake.api();
    const std::uint64_t site   = fixture.fake.address(0x10);

    const auto installed = script::hook::install(hook_spec, memory, site);
    CHECK_FALSE(installed.has_value());
    CHECK(installed.error().find("are not the expected instruction") != std::string::npos);
    // Nothing was mapped or written.
    CHECK(fixture.fake.write_count == 0);
    CHECK(fixture.fake.allocations.empty());
}

TEST_CASE("hook::install frees and refuses a cave out of the site's reach", "[script][hook]")
{
    ScriptFixture            fixture(0x200);
    const script::hook::Spec hook_spec = simple_spec();
    prime_site(fixture.fake);
    fixture.fake.forced_allocation = 0x7F0000000000ull;

    MemoryApi           memory = fixture.fake.api();
    const std::uint64_t site   = fixture.fake.address(0x10);

    const auto installed = script::hook::install(hook_spec, memory, site);
    CHECK_FALSE(installed.has_value());
    CHECK(installed.error().find("out of the site's jump reach") != std::string::npos);
    CHECK(fixture.fake.allocations.empty());
    // The site was never touched.
    CHECK(fixture.fake.write_count == 0);
}

TEST_CASE("hook::install frees the cave when the site write is refused", "[script][hook]")
{
    ScriptFixture            fixture(0x200);
    const script::hook::Spec hook_spec = simple_spec();
    prime_site(fixture.fake);

    MemoryApi           memory   = fixture.fake.api();
    const std::uint64_t site     = fixture.fake.address(0x10);
    fixture.fake.refuse_write_at = site;

    const auto installed = script::hook::install(hook_spec, memory, site);
    CHECK_FALSE(installed.has_value());
    CHECK(installed.error().find("write refused") != std::string::npos);
    // The half-written mapping is gone again.
    CHECK(fixture.fake.allocations.empty());
}

namespace
{
    // The window a generated hook patches: eight whole bytes at offset 0x10.
    constexpr std::size_t kHookSiteOffset = 0x10;
    constexpr const char* kHookPattern    = "48 83 EC 28 48 89 D8 48";

    std::vector<std::byte> original_window()
    {
        return {std::byte {0x48},
                std::byte {0x83},
                std::byte {0xEC},
                std::byte {0x28},
                std::byte {0x48},
                std::byte {0x89},
                std::byte {0xD8},
                std::byte {0x48}};
    }

    void prime_hook_site(FakeMemory& fake)
    {
        fake.put(kHookSiteOffset,
                 {0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xD8, 0x48, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90});
    }

    // The facts table and the two lifecycle hooks a `Hook Instruction...`
    // prefill hands the runtime, written by hand so the engine path is tested
    // before the generator emits it.
    std::string hook_chunk(std::string_view pattern    = kHookPattern,
                           std::string_view trampoline = "SUB RSP, 0x28",
                           std::string_view arguments  = {})
    {
        std::string chunk = "local kHook = {\n";
        chunk += "    site_name = \"hook_site_test\",\n";
        chunk += "    cave_name = \"hook_cave_test\",\n";
        chunk += "    module = \"test.so\",\n";
        chunk += "    pattern = \"" + std::string {pattern} + "\",\n";
        chunk += "    offset = 0,\n";
        chunk += "    original = string.char(0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xD8, 0x48),\n";
        chunk += "    code = \"nop\",\n";
        chunk += "    trampoline = \"" + std::string {trampoline} + "\",\n";
        if (!arguments.empty())
        {
            chunk += "    trampoline_args = " + std::string {arguments} + ",\n";
        }
        chunk += "}\n";
        chunk += "function activate() return hook.install(kHook) end\n";
        chunk += "function deactivate() return hook.remove(kHook) end\n";
        return chunk;
    }
} // namespace

TEST_CASE("hook.install and hook.remove round-trip through a Lua chunk", "[script][hook]")
{
    ScriptFixture fixture(0x200);
    prime_hook_site(fixture.fake);

    const std::string chunk = hook_chunk();

    const script::LifecycleResult activated = fixture.engine.run_lifecycle(chunk, "activate");
    INFO(activated.message);
    REQUIRE(activated.ok);
    CHECK(activated.message.empty());

    // One cave, near the site, sized from the payload (1 + 4 + 5 rounded up).
    REQUIRE(fixture.fake.allocations.size() == 1);
    const std::uint64_t cave = fixture.fake.allocations.begin()->first;
    CHECK(fixture.fake.allocations.at(cave) == 16);
    REQUIRE(fixture.fake.allocate_requests.size() == 1);
    CHECK(fixture.fake.allocate_requests.front().first == fixture.fake.address(kHookSiteOffset));
    CHECK(fixture.fake.write_count == 2);

    // The site holds the jump and NOP padding; the cave holds the payload.
    const auto site_offset = static_cast<std::size_t>(fixture.fake.address(kHookSiteOffset) - kScriptBase);
    CHECK(fixture.fake.bytes[site_offset] == std::byte {0xE9});
    for (std::size_t offset = 5; offset < original_window().size(); ++offset)
    {
        CHECK(fixture.fake.bytes[site_offset + offset] == std::byte {0x90});
    }
    const auto cave_offset = static_cast<std::size_t>(cave - kScriptBase);
    CHECK(fixture.fake.bytes[cave_offset] == std::byte {0x90});     // the `nop` stub
    CHECK(fixture.fake.bytes[cave_offset + 1] == std::byte {0x48}); // SUB RSP, 0x28
    CHECK(fixture.fake.bytes[cave_offset + 5] == std::byte {0xE9}); // the jump back

    const script::LifecycleResult deactivated = fixture.engine.run_lifecycle(chunk, "deactivate");
    INFO(deactivated.message);
    REQUIRE(deactivated.ok);
    CHECK(deactivated.message.empty());

    // The originals are back and the cave is freed.
    const std::vector<std::byte> original = original_window();
    for (std::size_t offset = 0; offset < original.size(); ++offset)
    {
        CHECK(fixture.fake.bytes[site_offset + offset] == original[offset]);
    }
    CHECK(fixture.fake.allocations.empty());
}

TEST_CASE("hook.install refuses a pattern that is not unique", "[script][hook]")
{
    ScriptFixture fixture(0x200);
    prime_hook_site(fixture.fake);
    fixture.fake.put(0x40, {0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xD8, 0x48});

    const script::LifecycleResult activated = fixture.engine.run_lifecycle(hook_chunk(), "activate");
    CHECK_FALSE(activated.ok);
    CHECK(activated.message.find("matches more than one place") != std::string::npos);
    // Nothing was mapped or written.
    CHECK(fixture.fake.write_count == 0);
    CHECK(fixture.fake.allocations.empty());
}

TEST_CASE("hook.install reports a pattern it cannot find", "[script][hook]")
{
    ScriptFixture fixture(0x200);
    prime_hook_site(fixture.fake);

    const script::LifecycleResult activated = fixture.engine.run_lifecycle(hook_chunk("de ad be ef"), "activate");
    CHECK_FALSE(activated.ok);
    CHECK(activated.message.find("was not found") != std::string::npos);
    CHECK(fixture.fake.allocations.empty());
}

TEST_CASE("hook.install restricts the scan to the named module", "[script][hook]")
{
    ScriptFixture fixture(0x200);
    prime_hook_site(fixture.fake);
    // A second copy in another module: the module field keeps it out of the walk.
    fixture.fake.regions = {
        MemoryRegion {       .base = kScriptBase, .size = 0x80, .readable = true,  .module = "test.so"},
        MemoryRegion {.base = kScriptBase + 0x80, .size = 0x80, .readable = true, .module = "other.so"},
    };
    fixture.fake.put(0x90, {0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xD8, 0x48});

    const script::LifecycleResult activated = fixture.engine.run_lifecycle(hook_chunk(), "activate");
    INFO(activated.message);
    REQUIRE(activated.ok);
    CHECK(fixture.fake.allocations.size() == 1);
}

TEST_CASE("hook.install forwards site-relative trampoline arguments", "[script][hook]")
{
    ScriptFixture fixture(0x200);
    prime_hook_site(fixture.fake);

    const script::LifecycleResult activated =
        fixture.engine.run_lifecycle(hook_chunk(kHookPattern, "MOV EAX, 0x%X", "{ 0x2000 }"), "activate");
    INFO(activated.message);
    REQUIRE(activated.ok);

    REQUIRE(fixture.fake.allocations.size() == 1);
    const std::uint64_t cave        = fixture.fake.allocations.begin()->first;
    const auto          cave_offset = static_cast<std::size_t>(cave - kScriptBase);
    // site (0x1010) + 0x2000 = 0x3010, encoded as `MOV EAX, 0x3010`.
    CHECK(fixture.fake.bytes[cave_offset] == std::byte {0x90});
    CHECK(fixture.fake.bytes[cave_offset + 1] == std::byte {0xB8});
    CHECK(fixture.fake.bytes[cave_offset + 2] == std::byte {0x10});
    CHECK(fixture.fake.bytes[cave_offset + 3] == std::byte {0x30});
    CHECK(fixture.fake.bytes[cave_offset + 4] == std::byte {0x00});
    CHECK(fixture.fake.bytes[cave_offset + 5] == std::byte {0x00});
}

TEST_CASE("hook.install refuses a target that cannot allocate", "[script][hook]")
{
    ScriptFixture fixture(0x200);
    prime_hook_site(fixture.fake);

    MemoryApi memory  = fixture.fake.api();
    memory.allocate   = nullptr;
    memory.deallocate = nullptr;
    Engine engine(memory, fixture.symbols.api());

    const script::LifecycleResult activated = engine.run_lifecycle(hook_chunk(), "activate");
    CHECK_FALSE(activated.ok);
    CHECK(activated.message.find("cannot allocate memory") != std::string::npos);
}

TEST_CASE("deactivate refuses a site label it never saw", "[script][hook]")
{
    ScriptFixture fixture(0x200);
    prime_hook_site(fixture.fake);

    const script::LifecycleResult deactivated = fixture.engine.run_lifecycle(hook_chunk(), "deactivate");
    CHECK_FALSE(deactivated.ok);
    CHECK(deactivated.message.find("this hook's site is not known anymore") != std::string::npos);
}

TEST_CASE("hook.install raises on a malformed facts table", "[script][hook]")
{
    ScriptFixture fixture(0x200);

    const RunResult not_a_table = fixture.engine.run(R"(hook.install("nope"))");
    CHECK_FALSE(not_a_table.ok);
    CHECK(not_a_table.error.find("hook.install: the argument must be a table") != std::string::npos);

    const RunResult missing = fixture.engine.run(R"(hook.install({ site_name = "a", cave_name = "b" }))");
    CHECK_FALSE(missing.ok);
    CHECK(missing.error.find("hook.install: the pattern field must be a string") != std::string::npos);
}
