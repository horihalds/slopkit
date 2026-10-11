#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

TEST_CASE("the module spans keep file-backed images sorted by base", "[ui]")
{
    SECTION("keeps file-backed images sorted by base")
    {
        slopkit::ui::ModuleSpans spans;
        CHECK(spans.empty());
        CHECK(spans.containing(0x1000) == nullptr);
        CHECK(spans.find_by_name("low") == nullptr);

        std::vector<slopkit::process::ModuleInfo> modules;
        modules.push_back(module_image("low", 0x2000, 0x1000));
        modules.push_back(module_image("high", 0x5000, 0x100));
        slopkit::process::ModuleInfo anonymous;
        anonymous.base = 0x3000;
        anonymous.size = 0x1000;
        modules.push_back(anonymous);
        // Deliberately out of order; the map sorts by base.
        modules.push_back(module_image("first", 0x1000, 0x800));

        spans.set_modules(modules);

        CHECK_FALSE(spans.empty());
        REQUIRE(spans.containing(0x1000) != nullptr);
        CHECK(spans.containing(0x1000)->name == "first");
        CHECK(spans.containing(0x17FF)->name == "first");
        CHECK(spans.containing(0x1800) == nullptr);
        CHECK(spans.containing(0x2FFF)->name == "low");
        CHECK(spans.containing(0x3000) == nullptr);
        CHECK(spans.containing(0x0FFF) == nullptr);

        REQUIRE(spans.find_by_name("LOW") != nullptr);
        CHECK(spans.find_by_name("High")->base == 0x5000);
        CHECK(spans.find_by_name("missing") == nullptr);

        spans.clear();
        CHECK(spans.empty());
    }

    SECTION("marks the main image")
    {
        slopkit::ui::ModuleSpans spans;
        CHECK(spans.main() == nullptr);

        std::vector<slopkit::process::ModuleInfo> modules;
        modules.push_back(module_image("low", 0x2000, 0x1000));
        modules.push_back(module_image("lib", 0x8000, 0x100));
        slopkit::process::ModuleInfo game = module_image("game", 0x5000, 0x1000);
        game.is_main                      = true;
        modules.push_back(game);

        spans.set_modules(modules);

        REQUIRE(spans.main() != nullptr);
        CHECK(spans.main()->name == "game");
        CHECK(spans.main()->base == 0x5000);
        CHECK(spans.main()->is_main);
        CHECK(spans.containing(0x5000)->is_main);
        CHECK_FALSE(spans.containing(0x2000)->is_main);
        CHECK_FALSE(spans.containing(0x8000)->is_main);

        const std::vector<slopkit::process::ModuleInfo> unflagged {module_image("high", 0x5000, 0x100),
                                                                   module_image("low", 0x2000, 0x1000)};
        spans.set_modules(unflagged);
        REQUIRE(spans.main() != nullptr);
        CHECK(spans.main()->name == "low");
        CHECK(spans.main()->is_main);

        spans.clear();
        CHECK(spans.main() == nullptr);
    }

    SECTION("labels a module without a name from its path")
    {
        slopkit::process::ModuleInfo module = module_image("", 0x4000, 0x100);
        module.path                         = "/usr/lib/libc.so.6";
        const std::vector<slopkit::process::ModuleInfo> modules {module};

        slopkit::ui::ModuleSpans spans;
        spans.set_modules(modules);
        REQUIRE(spans.containing(0x4000) != nullptr);
        CHECK(spans.containing(0x4000)->name == "libc.so.6");
        CHECK(slopkit::ui::format_module_relative(*spans.containing(0x4000), 0x4000)
              == QStringLiteral("libc.so.6+0x0"));
    }
}

TEST_CASE("long module names are shortened before the extension", "[ui]")
{
    using slopkit::ui::elide_module_name;

    CHECK(elide_module_name("DyingLightGame_TheBeast_x64_rwdi.exe") == QStringLiteral("DyingL..._rwdi.exe"));
    CHECK(elide_module_name("SixteenCharsHere.dll") == QStringLiteral("SixteenCharsHere.dll"));
    CHECK(elide_module_name("SeventeenCharsHerr.dll") == QStringLiteral("Sevent...sHerr.dll"));
    CHECK(elide_module_name("a_very_long_module_name_without_dot") == QStringLiteral("a_very...t_dot"));
    CHECK(elide_module_name(".hidden") == QStringLiteral(".hidden"));
    CHECK(elide_module_name(".a_very_long_hidden_file") == QStringLiteral(".a_ver..._file"));
    CHECK(elide_module_name("libc.so.6") == QStringLiteral("libc.so.6"));
    CHECK(elide_module_name("") == QString());
}

TEST_CASE("module-relative addresses render as name+HEX", "[ui]")
{
    const slopkit::ui::ModuleSpan span {"app", {}, 0x1000, 0x2000};

    CHECK(slopkit::ui::format_module_relative(span, 0x1000) == QStringLiteral("app+0x0"));
    CHECK(slopkit::ui::format_module_relative(span, 0x1040) == QStringLiteral("app+0x40"));
    CHECK(slopkit::ui::format_module_relative(span, 0x1A2B) == QStringLiteral("app+0xA2B"));
    CHECK(slopkit::ui::format_module_relative(span, 0x1040).contains(QStringLiteral("0x")));
}

TEST_CASE("absolute addresses render as 0xHEX", "[ui]")
{
    CHECK(slopkit::ui::format_absolute(0) == QStringLiteral("0x0"));
    CHECK(slopkit::ui::format_absolute(0x1040) == QStringLiteral("0x1040"));
    CHECK(slopkit::ui::format_absolute(0x5000000) == QStringLiteral("0x5000000"));
    CHECK(slopkit::ui::format_absolute(0xabcdef) == QStringLiteral("0xABCDEF"));
}

TEST_CASE("padded hex zero-fills to the requested width", "[ui]")
{
    CHECK(slopkit::ui::format_padded_hex(0) == QStringLiteral("0x0000000000000000"));
    CHECK(slopkit::ui::format_padded_hex(0x41) == QStringLiteral("0x0000000000000041"));
    CHECK(slopkit::ui::format_padded_hex(0xFFFFFFFFFFFFFFFF) == QStringLiteral("0xFFFFFFFFFFFFFFFF"));
    CHECK(slopkit::ui::format_padded_hex(0x12345, 2) == QStringLiteral("0x12345"));
    CHECK(slopkit::ui::format_padded_hex(0x41, 2) == QStringLiteral("0x41"));
    CHECK(slopkit::ui::format_padded_hex(0x9, 2) == QStringLiteral("0x09"));
}

TEST_CASE("the pane and cell address helpers pick their fallback", "[ui]")
{
    const std::vector<slopkit::process::ModuleInfo> modules {module_image("app", 0x1000, 0x1000)};
    slopkit::ui::ModuleSpans                        spans;
    spans.set_modules(modules);

    CHECK(slopkit::ui::format_pane_address(slopkit::ui::AddressMode::module_relative, spans, 0x1040)
          == QStringLiteral("app+0x40"));
    CHECK(slopkit::ui::format_cell_address(slopkit::ui::AddressMode::module_relative, spans, 0x1040)
          == QStringLiteral("app+0x40"));

    CHECK(slopkit::ui::format_pane_address(slopkit::ui::AddressMode::module_relative, spans, 0x5000000)
          == QStringLiteral("0x0000000005000000"));
    CHECK(slopkit::ui::format_cell_address(slopkit::ui::AddressMode::module_relative, spans, 0x5000000)
          == QStringLiteral("0x5000000"));

    CHECK(slopkit::ui::format_pane_address(slopkit::ui::AddressMode::absolute, spans, 0x1040)
          == QStringLiteral("0x0000000000001040"));
    CHECK(slopkit::ui::format_cell_address(slopkit::ui::AddressMode::absolute, spans, 0x1040)
          == QStringLiteral("0x1040"));
}

TEST_CASE("module-relative text falls back in absolute mode and outside spans", "[ui]")
{
    const std::vector<slopkit::process::ModuleInfo> modules {module_image("app", 0x1000, 0x1000)};
    slopkit::ui::ModuleSpans                        spans;
    spans.set_modules(modules);

    const auto relative = slopkit::ui::module_relative_text(slopkit::ui::AddressMode::module_relative, spans, 0x1040);
    REQUIRE(relative.has_value());
    CHECK(*relative == QStringLiteral("app+0x40"));

    CHECK_FALSE(
        slopkit::ui::module_relative_text(slopkit::ui::AddressMode::module_relative, spans, 0x5000000).has_value());
    CHECK_FALSE(slopkit::ui::module_relative_text(slopkit::ui::AddressMode::absolute, spans, 0x1040).has_value());

    slopkit::ui::ModuleSpans empty;
    CHECK_FALSE(
        slopkit::ui::module_relative_text(slopkit::ui::AddressMode::module_relative, empty, 0x1040).has_value());
}

TEST_CASE("a shortened module label still has an untruncated spelling", "[ui]")
{
    const std::string                               long_name = "DyingLightGame_TheBeast_x64_rwdi.exe";
    const std::vector<slopkit::process::ModuleInfo> modules {module_image(long_name, 0x1000, 0x10000)};
    slopkit::ui::ModuleSpans                        spans;
    spans.set_modules(modules);

    const slopkit::ui::ModuleSpan* span = spans.containing(0x1000 + 0x1A2B);
    REQUIRE(span != nullptr);
    CHECK(span->name == long_name);
    CHECK(span->label() == "DyingL..._rwdi.exe");

    CHECK(slopkit::ui::format_module_relative(*span, 0x1000 + 0x1A2B) == QStringLiteral("DyingL..._rwdi.exe+0x1A2B"));
    CHECK(slopkit::ui::format_module_relative_full(*span, 0x1000 + 0x1A2B)
          == QStringLiteral("DyingLightGame_TheBeast_x64_rwdi.exe+0x1A2B"));
    CHECK(slopkit::ui::format_pane_address(slopkit::ui::AddressMode::module_relative, spans, 0x1000 + 0x1A2B)
          == QStringLiteral("DyingL..._rwdi.exe+0x1A2B"));
    CHECK(slopkit::ui::format_cell_address(slopkit::ui::AddressMode::module_relative, spans, 0x1000 + 0x1A2B)
          == QStringLiteral("DyingL..._rwdi.exe+0x1A2B"));
    CHECK(slopkit::ui::format_pane_address_full(slopkit::ui::AddressMode::module_relative, spans, 0x1000 + 0x1A2B)
          == QStringLiteral("DyingLightGame_TheBeast_x64_rwdi.exe+0x1A2B"));
    CHECK(slopkit::ui::format_cell_address_full(slopkit::ui::AddressMode::module_relative, spans, 0x1000 + 0x1A2B)
          == QStringLiteral("DyingLightGame_TheBeast_x64_rwdi.exe+0x1A2B"));
}

TEST_CASE("a full module address still resolves when pasted back", "[ui]")
{
    const std::vector<slopkit::process::ModuleInfo> modules {
        module_image("DyingLightGame_TheBeast_x64_rwdi.exe", 0x1000, 0x10000)};
    slopkit::ui::ModuleSpans spans;
    spans.set_modules(modules);

    const QString full =
        slopkit::ui::format_cell_address_full(slopkit::ui::AddressMode::module_relative, spans, 0x1000 + 0x1A2B);
    const std::string text = full.toStdString();
    CHECK(slopkit::ui::parse_address_text(text, spans) == std::optional<std::uint64_t> {0x1000 + 0x1A2B});
    // The shortened spelling is display-only and never resolves.
    CHECK_FALSE(slopkit::ui::parse_address_text("DyingL..._rwdi.exe+0x1A2B", spans).has_value());
}

TEST_CASE("address text parses both absolute and module-relative forms", "[ui]")
{
    const std::vector<slopkit::process::ModuleInfo> modules {module_image("low", 0x1000, 0x1000)};
    slopkit::ui::ModuleSpans                        spans;
    spans.set_modules(modules);

    CHECK(slopkit::ui::parse_address_text("0x1040", spans) == std::optional<std::uint64_t> {0x1040});
    CHECK(slopkit::ui::parse_address_text("1040", spans) == std::optional<std::uint64_t> {0x1040});
    CHECK(slopkit::ui::parse_address_text("#1040", spans) == std::optional<std::uint64_t> {1040});
    CHECK(slopkit::ui::parse_address_text("0000000000001040", spans) == std::optional<std::uint64_t> {0x1040});

    CHECK(slopkit::ui::parse_address_text("LOW+40", spans) == std::optional<std::uint64_t> {0x1040});
    CHECK(slopkit::ui::parse_address_text("low+0x40", spans) == std::optional<std::uint64_t> {0x1040});
    CHECK(slopkit::ui::parse_address_text("low+0", spans) == std::optional<std::uint64_t> {0x1000});

    CHECK(slopkit::ui::parse_address_text("low", spans) == std::optional<std::uint64_t> {0x1000});
    CHECK(slopkit::ui::parse_address_text("LOW", spans) == std::optional<std::uint64_t> {0x1000});

    CHECK_FALSE(slopkit::ui::parse_address_text("missing+40", spans).has_value());
    CHECK_FALSE(slopkit::ui::parse_address_text("missing", spans).has_value());
    CHECK_FALSE(slopkit::ui::parse_address_text("low+bogus", spans).has_value());
    CHECK_FALSE(slopkit::ui::parse_address_text("not an address", spans).has_value());

    CHECK_FALSE(slopkit::ui::parse_address_text("[low+40]", spans).has_value());
    CHECK_FALSE(slopkit::ui::parse_address_text("[low]+40", spans).has_value());
    CHECK(slopkit::ui::parse_address_text("low+40", spans) == std::optional<std::uint64_t> {0x1040});
}

TEST_CASE("address text resolves symbol names", "[ui]")
{
    const std::vector<slopkit::process::ModuleInfo> modules {module_image("low", 0x1000, 0x1000)};
    slopkit::ui::ModuleSpans                        spans;
    spans.set_modules(modules);
    const std::vector<slopkit::expr::SymbolRef> symbols {
        {      "hp", 0x1337},
        {     "low", 0x9999},
        {"deadbeef", 0xABCD},
    };

    CHECK(slopkit::ui::parse_address_text("hp", spans, symbols) == std::optional<std::uint64_t> {0x1337});
    CHECK(slopkit::ui::parse_address_text("HP", spans, symbols) == std::optional<std::uint64_t> {0x1337});
    CHECK(slopkit::ui::parse_address_text("hp+10", spans, symbols) == std::optional<std::uint64_t> {0x1347});
    CHECK(slopkit::ui::parse_address_text("low", spans, symbols) == std::optional<std::uint64_t> {0x1000});
    CHECK(slopkit::ui::parse_address_text("deadbeef", spans, symbols) == std::optional<std::uint64_t> {0xABCD});

    CHECK(slopkit::ui::parse_address_text("deadbeef", spans) == std::optional<std::uint64_t> {0xDEADBEEF});
    CHECK_FALSE(slopkit::ui::parse_address_text("hp", spans).has_value());
}
