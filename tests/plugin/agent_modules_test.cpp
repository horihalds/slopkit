#include <catch2/catch.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "platform/linux/procfs.hpp"
#include "plugins/wine_proton/agent_client.hpp"
#include "plugins/wine_proton/agent_modules.hpp"

namespace
{
    using slopkit::platform::MappedRegion;
    using slopkit::plugins::wine_proton::AgentModuleEntry;
    using slopkit::plugins::wine_proton::decode_utf16;
    using slopkit::plugins::wine_proton::merge_modules;
    using slopkit::process::ModuleKind;

    AgentModuleEntry image(std::uint64_t base, std::uint64_t size, std::uint64_t entry, bool main, std::string name)
    {
        AgentModuleEntry record;
        record.base  = base;
        record.size  = size;
        record.entry = entry;
        record.main  = main;
        record.name  = std::move(name);
        return record;
    }

    MappedRegion mapping(std::uint64_t start, std::uint64_t end, std::uint64_t offset, std::string path)
    {
        MappedRegion region;
        region.start  = start;
        region.end    = end;
        region.offset = offset;
        region.path   = std::move(path);
        return region;
    }
} // namespace

TEST_CASE("the module merge takes the image from the loader and the path from procfs", "[wine_proton][agent]")
{
    const std::vector<AgentModuleEntry> images {
        image(0x140000000ull, 0x1000, 0x140001000ull, true, "game.exe"),
        image(0x7B000000ull, 0x2000, 0x7B001234ull, false, "kernel32.dll"),
    };
    const std::vector<MappedRegion> maps {
        mapping(0x140000000ull, 0x140004000ull, 0, "/games/game.exe"),
        mapping(0x7B000000ull, 0x7B008000ull, 0x3000, "/usr/lib/wine/x86_64-windows/kernel32.dll"),
    };

    const auto merged = merge_modules(images, maps);
    REQUIRE(merged.size() == 2);

    // The list is ascending by base, so kernel32 (0x7B000000) precedes the
    // 0x140000000 image even though the loader listed the main image first.
    CHECK(merged[0].name == "kernel32.dll");
    CHECK_FALSE(merged[0].is_main);
    CHECK(merged[0].kind == ModuleKind::pe);
    // The overlapping procfs line supplies the on-disk identity.
    CHECK(merged[0].path == "/usr/lib/wine/x86_64-windows/kernel32.dll");
    CHECK(merged[0].offset == 0x3000);

    // The loader supplies the base, image size, entry point, name and main flag.
    CHECK(merged[1].base == 0x140000000ull);
    CHECK(merged[1].size == 0x1000);
    CHECK(merged[1].entry == 0x140001000ull);
    CHECK(merged[1].name == "game.exe");
    CHECK(merged[1].is_main);
    CHECK(merged[1].kind == ModuleKind::pe);
    CHECK(merged[1].path == "/games/game.exe");
    CHECK(merged[1].offset == 0);

    CHECK(merged[0].base < merged[1].base);
}

TEST_CASE("a loader image with no procfs counterpart keeps an empty path", "[wine_proton][agent]")
{
    const std::vector<AgentModuleEntry> images {image(0x180000000ull, 0x4000, 0, false, "cached.dll")};
    const std::vector<MappedRegion>     maps {mapping(0x1000, 0x2000, 0, "/tmp/other")};

    const auto merged = merge_modules(images, maps);
    REQUIRE(merged.size() == 1);
    CHECK(merged[0].path.empty());
    CHECK(merged[0].offset == 0);
    CHECK(merged[0].entry == 0);
}

TEST_CASE("the module merge sorts by base and carries the agent's main flag", "[wine_proton][agent]")
{
    const std::vector<AgentModuleEntry> images {
        image(0x9000, 0x1000, 0, false, "z.dll"),
        image(0x3000, 0x1000, 0, false, "a.dll"),
        image(0x6000, 0x1000, 0, true, "main.exe"),
    };

    const auto merged = merge_modules(images, {});
    REQUIRE(merged.size() == 3);
    CHECK(merged[0].name == "a.dll");
    CHECK(merged[1].name == "main.exe");
    CHECK(merged[2].name == "z.dll");
    CHECK_FALSE(merged[0].is_main);
    CHECK(merged[1].is_main);
    CHECK_FALSE(merged[2].is_main);
}

TEST_CASE("an empty agent list produces an empty module list", "[wine_proton][agent]")
{
    const std::vector<MappedRegion> maps {mapping(0x1000, 0x2000, 0, "/tmp/x")};
    CHECK(merge_modules(std::span<const AgentModuleEntry> {}, maps).empty());
}

TEST_CASE("the UTF-16 name decoder handles ASCII, surrogate pairs and truncation", "[wine_proton][agent]")
{
    SECTION("ASCII")
    {
        const auto  text = u"kernel32.dll";
        std::string out;
        decode_utf16(std::span<const char16_t>(text, 12), out);
        CHECK(out == "kernel32.dll");
    }

    SECTION("a non-BMP surrogate pair becomes one UTF-8 code point")
    {
        const char16_t text[] = {0xD83D, 0xDE00}; // U+1F600
        std::string    out;
        decode_utf16(text, out);
        CHECK(out == "\xF0\x9F\x98\x80");
    }

    SECTION("a high surrogate with a non-low successor is replaced")
    {
        const char16_t text[] = {0xD83D, u'A'};
        std::string    out;
        decode_utf16(text, out);
        CHECK(out
              == "\xEF\xBF\xBD"
                 "A");
    }

    SECTION("a high surrogate at the end is replaced without reading past the span")
    {
        const char16_t text[] = {u'A', 0xD83D};
        std::string    out;
        decode_utf16(text, out);
        CHECK(out == "A\xEF\xBF\xBD");
    }

    SECTION("a lone low surrogate is replaced")
    {
        const char16_t text[] = {0xDE00};
        std::string    out;
        decode_utf16(text, out);
        CHECK(out == "\xEF\xBF\xBD");
    }

    SECTION("an empty name stays empty")
    {
        std::string out;
        decode_utf16({}, out);
        CHECK(out.empty());
    }
}
