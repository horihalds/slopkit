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
        // Not file-backed: dropped from the map.
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
        CHECK(spans.containing(0x1800) == nullptr); // Half-open: base + size is outside.
        CHECK(spans.containing(0x2FFF)->name == "low");
        CHECK(spans.containing(0x3000) == nullptr); // The anonymous mapping never labels an address.
        CHECK(spans.containing(0x0FFF) == nullptr);

        // The name lookup is case-insensitive.
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
        // The flagged main image is not the lowest-based one.
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

        // Without a flagged image the lowest-based image becomes the main one.
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
        CHECK(slopkit::ui::format_module_relative(*spans.containing(0x4000), 0x4000) == QStringLiteral("libc.so.6+0"));
    }
}

TEST_CASE("module-relative addresses render as name+HEX", "[ui]")
{
    const slopkit::ui::ModuleSpan span {"app", 0x1000, 0x2000};

    CHECK(slopkit::ui::format_module_relative(span, 0x1000) == QStringLiteral("app+0"));
    CHECK(slopkit::ui::format_module_relative(span, 0x1040) == QStringLiteral("app+40"));
    CHECK(slopkit::ui::format_module_relative(span, 0x1A2B) == QStringLiteral("app+A2B"));
    // No 0x prefix and no leading zeros.
    CHECK_FALSE(slopkit::ui::format_module_relative(span, 0x1040).contains(QStringLiteral("0x")));
}

TEST_CASE("absolute addresses render as bare HEX", "[ui]")
{
    CHECK(slopkit::ui::format_absolute(0) == QStringLiteral("0"));
    CHECK(slopkit::ui::format_absolute(0x1040) == QStringLiteral("1040"));
    CHECK(slopkit::ui::format_absolute(0x5000000) == QStringLiteral("5000000"));
    // Upper-case hex, no leading zeros, no 0x prefix.
    CHECK(slopkit::ui::format_absolute(0xabcdef) == QStringLiteral("ABCDEF"));
}

TEST_CASE("padded hex zero-fills to the requested width", "[ui]")
{
    CHECK(slopkit::ui::format_padded_hex(0) == QStringLiteral("0000000000000000"));
    CHECK(slopkit::ui::format_padded_hex(0x41) == QStringLiteral("0000000000000041"));
    CHECK(slopkit::ui::format_padded_hex(0xFFFFFFFFFFFFFFFF) == QStringLiteral("FFFFFFFFFFFFFFFF"));
    // A value wider than the asked-for width keeps all of its digits.
    CHECK(slopkit::ui::format_padded_hex(0x12345, 2) == QStringLiteral("12345"));
    // The Access Watch operand's 2-digit byte and displacement widths.
    CHECK(slopkit::ui::format_padded_hex(0x41, 2) == QStringLiteral("41"));
    CHECK(slopkit::ui::format_padded_hex(0x9, 2) == QStringLiteral("09"));
}

TEST_CASE("the pane and cell address helpers pick their fallback", "[ui]")
{
    const std::vector<slopkit::process::ModuleInfo> modules {module_image("app", 0x1000, 0x1000)};
    slopkit::ui::ModuleSpans                        spans;
    spans.set_modules(modules);

    // Inside a span both forms are module-relative.
    CHECK(slopkit::ui::format_pane_address(slopkit::ui::AddressMode::module_relative, spans, 0x1040)
          == QStringLiteral("app+40"));
    CHECK(slopkit::ui::format_cell_address(slopkit::ui::AddressMode::module_relative, spans, 0x1040)
          == QStringLiteral("app+40"));

    // Outside every span the pane keeps the fixed width, the cell stays compact.
    CHECK(slopkit::ui::format_pane_address(slopkit::ui::AddressMode::module_relative, spans, 0x5000000)
          == QStringLiteral("0000000005000000"));
    CHECK(slopkit::ui::format_cell_address(slopkit::ui::AddressMode::module_relative, spans, 0x5000000)
          == QStringLiteral("5000000"));

    // Absolute mode never goes module-relative.
    CHECK(slopkit::ui::format_pane_address(slopkit::ui::AddressMode::absolute, spans, 0x1040)
          == QStringLiteral("0000000000001040"));
    CHECK(slopkit::ui::format_cell_address(slopkit::ui::AddressMode::absolute, spans, 0x1040)
          == QStringLiteral("1040"));
}

TEST_CASE("module-relative text falls back in absolute mode and outside spans", "[ui]")
{
    const std::vector<slopkit::process::ModuleInfo> modules {module_image("app", 0x1000, 0x1000)};
    slopkit::ui::ModuleSpans                        spans;
    spans.set_modules(modules);

    const auto relative = slopkit::ui::module_relative_text(slopkit::ui::AddressMode::module_relative, spans, 0x1040);
    REQUIRE(relative.has_value());
    CHECK(*relative == QStringLiteral("app+40"));

    // Outside every span there is no module-relative text, whatever the mode.
    CHECK_FALSE(
        slopkit::ui::module_relative_text(slopkit::ui::AddressMode::module_relative, spans, 0x5000000).has_value());
    // The absolute mode never produces module-relative text.
    CHECK_FALSE(slopkit::ui::module_relative_text(slopkit::ui::AddressMode::absolute, spans, 0x1040).has_value());

    slopkit::ui::ModuleSpans empty;
    CHECK_FALSE(
        slopkit::ui::module_relative_text(slopkit::ui::AddressMode::module_relative, empty, 0x1040).has_value());
}

TEST_CASE("address text parses both absolute and module-relative forms", "[ui]")
{
    const std::vector<slopkit::process::ModuleInfo> modules {module_image("low", 0x1000, 0x1000)};
    slopkit::ui::ModuleSpans                        spans;
    spans.set_modules(modules);

    // Absolute forms still go through scan::parse_address: bare digits are hex,
    // `0x…` is hex and `#…` is decimal.
    CHECK(slopkit::ui::parse_address_text("0x1040", spans) == std::optional<std::uint64_t> {0x1040});
    CHECK(slopkit::ui::parse_address_text("1040", spans) == std::optional<std::uint64_t> {0x1040});
    CHECK(slopkit::ui::parse_address_text("#1040", spans) == std::optional<std::uint64_t> {1040});
    // The zero-padded text the panes prefill round-trips as hex.
    CHECK(slopkit::ui::parse_address_text("0000000000001040", spans) == std::optional<std::uint64_t> {0x1040});

    // module+RVA: case-insensitive name, optional 0x on the RVA.
    CHECK(slopkit::ui::parse_address_text("LOW+40", spans) == std::optional<std::uint64_t> {0x1040});
    CHECK(slopkit::ui::parse_address_text("low+0x40", spans) == std::optional<std::uint64_t> {0x1040});
    CHECK(slopkit::ui::parse_address_text("low+0", spans) == std::optional<std::uint64_t> {0x1000});

    // A bare module name resolves to its base, case-insensitively.
    CHECK(slopkit::ui::parse_address_text("low", spans) == std::optional<std::uint64_t> {0x1000});
    CHECK(slopkit::ui::parse_address_text("LOW", spans) == std::optional<std::uint64_t> {0x1000});

    // An unknown module or an unparsable RVA yields nothing.
    CHECK_FALSE(slopkit::ui::parse_address_text("missing+40", spans).has_value());
    CHECK_FALSE(slopkit::ui::parse_address_text("missing", spans).has_value());
    CHECK_FALSE(slopkit::ui::parse_address_text("low+bogus", spans).has_value());
    CHECK_FALSE(slopkit::ui::parse_address_text("not an address", spans).has_value());
}
