#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

TEST_CASE("both themes define every colour role", "[ui]")
{
    const auto dark  = slopkit::ui::dark_theme();
    const auto light = slopkit::ui::light_theme();

    check_all_roles_defined(dark);
    check_all_roles_defined(light);

    CHECK(dark.background != light.background);
    CHECK(dark.surface != light.surface);
    CHECK(dark.text != light.text);
    CHECK(dark.accent != light.accent);
}

TEST_CASE("the syntax roles are defined and distinct", "[ui]")
{
    const auto dark  = slopkit::ui::dark_theme();
    const auto light = slopkit::ui::light_theme();

    for (const auto& theme : {dark, light})
    {
        CHECK(theme.syntax_register != theme.text);
        CHECK(theme.syntax_immediate != theme.text);
        CHECK(theme.syntax_module != theme.text);
        CHECK(theme.syntax_register != theme.syntax_immediate);
        CHECK(theme.syntax_immediate != theme.syntax_module);
        CHECK(theme.syntax_register != theme.syntax_module);
    }

    CHECK(dark.syntax_register != light.syntax_register);
    CHECK(dark.syntax_immediate != light.syntax_immediate);
    CHECK(dark.syntax_module != light.syntax_module);
}

TEST_CASE("make_palette maps the colour roles onto the Qt palette", "[ui]")
{
    const auto     theme   = slopkit::ui::light_theme();
    const QPalette palette = slopkit::ui::make_palette(theme);

    CHECK(palette.color(QPalette::Window) == theme.background);
    CHECK(palette.color(QPalette::Base) == theme.surface);
    CHECK(palette.color(QPalette::Button) == theme.surface);
    CHECK(palette.color(QPalette::WindowText) == theme.text);
    CHECK(palette.color(QPalette::Text) == theme.text);
    CHECK(palette.color(QPalette::ButtonText) == theme.text);
    CHECK(palette.color(QPalette::Mid) == theme.border);
    CHECK(palette.color(QPalette::Highlight) == theme.accent);
    CHECK(palette.color(QPalette::HighlightedText) == theme.on_accent);
    CHECK(palette.color(QPalette::PlaceholderText) == theme.text_muted);
    CHECK(palette.color(QPalette::Disabled, QPalette::Text) == theme.text_muted);

    const QPalette dark = slopkit::ui::make_palette(slopkit::ui::dark_theme());
    CHECK(dark.color(QPalette::Window) != palette.color(QPalette::Window));
}

TEST_CASE("apply_theme installs the palette and caches the theme", "[ui]")
{
    application();

    slopkit::ui::apply_theme(slopkit::ui::light_theme());
    CHECK(slopkit::ui::active_theme().background == slopkit::ui::light_theme().background);
    CHECK(QGuiApplication::palette().color(QPalette::Window) == slopkit::ui::light_theme().background);

    slopkit::ui::apply_theme(slopkit::ui::dark_theme());
    CHECK(slopkit::ui::active_theme().background == slopkit::ui::dark_theme().background);
    CHECK(QGuiApplication::palette().color(QPalette::Window) == slopkit::ui::dark_theme().background);
}

TEST_CASE("themed widgets follow a theme switch", "[ui]")
{
    application();

    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    slopkit::ui::widgets::StatusLabel status;
    status.set_status(slopkit::ui::widgets::StatusKind::error, QStringLiteral("boom"));
    CHECK(status.palette().color(QPalette::WindowText) == slopkit::ui::dark_theme().error);

    slopkit::ui::widgets::PrimaryButton button(QStringLiteral("Scan"));
    CHECK(button.palette().color(QPalette::Button) == slopkit::ui::dark_theme().accent);

    // A theme switch changes the application palette, which Qt propagates to
    // every widget as a palette-change event; deliver it here so the check does
    // not depend on the event loop's timing.
    slopkit::ui::apply_theme(slopkit::ui::light_theme());
    QEvent palette_change(QEvent::PaletteChange);
    QCoreApplication::sendEvent(&status, &palette_change);
    QCoreApplication::sendEvent(&button, &palette_change);

    CHECK(status.palette().color(QPalette::WindowText) == slopkit::ui::light_theme().error);
    CHECK(button.palette().color(QPalette::Button) == slopkit::ui::light_theme().accent);
}
