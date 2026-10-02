#include <catch2/catch.hpp>

#include <imgui.h>

#include "ui/fonts.hpp"
#include "ui/scale.hpp"
#include "ui/theme.hpp"

namespace
{
    // Tests exercise ImGui types but never open a window, so a bare context is
    // enough.
    struct ContextGuard
    {
        ContextGuard()
        {
            ImGui::CreateContext();
        }

        ~ContextGuard()
        {
            ImGui::DestroyContext();
        }

        ContextGuard(const ContextGuard&)            = delete;
        ContextGuard& operator=(const ContextGuard&) = delete;
    };

    bool same_color(const ImVec4& lhs, const ImVec4& rhs)
    {
        return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z && lhs.w == rhs.w;
    }

    void check_all_roles_defined(const slopkit::ui::Theme& theme)
    {
        const ImVec4 roles[] = {theme.background,
                                theme.surface,
                                theme.surface_hover,
                                theme.text,
                                theme.text_muted,
                                theme.accent,
                                theme.accent_hover,
                                theme.accent_active,
                                theme.border,
                                theme.success,
                                theme.warning,
                                theme.error};
        for (const auto& role : roles)
        {
            CHECK(role.w > 0.0f);
        }
    }
} // namespace

TEST_CASE("both themes define every colour role", "[ui]")
{
    const auto dark  = slopkit::ui::dark_theme();
    const auto light = slopkit::ui::light_theme();

    check_all_roles_defined(dark);
    check_all_roles_defined(light);

    CHECK_FALSE(same_color(dark.background, light.background));
    CHECK_FALSE(same_color(dark.text, light.text));
    CHECK_FALSE(same_color(dark.surface, light.surface));
}

TEST_CASE("apply_theme maps colour roles onto the style", "[ui]")
{
    ContextGuard guard;

    ImGuiStyle style = slopkit::ui::make_base_style();
    const auto theme = slopkit::ui::light_theme();
    slopkit::ui::apply_theme(style, theme);

    CHECK(same_color(style.Colors[ImGuiCol_WindowBg], theme.background));
    CHECK(same_color(style.Colors[ImGuiCol_ChildBg], theme.background));
    CHECK(same_color(style.Colors[ImGuiCol_Text], theme.text));
    CHECK(same_color(style.Colors[ImGuiCol_TextDisabled], theme.text_muted));
    CHECK(same_color(style.Colors[ImGuiCol_FrameBg], theme.surface));
    CHECK(same_color(style.Colors[ImGuiCol_Border], theme.border));
    CHECK(same_color(style.Colors[ImGuiCol_Button], theme.surface));
    CHECK(same_color(style.Colors[ImGuiCol_CheckMark], theme.accent));

    CHECK(same_color(slopkit::ui::active_theme().accent, theme.accent));
}

TEST_CASE("scaling derives the active style from an unscaled base", "[ui]")
{
    ContextGuard guard;

    slopkit::ui::Scale scale;
    CHECK(scale.factor() == 1.0f);
    CHECK(scale.style().ItemSpacing.x == scale.base_style().ItemSpacing.x);

    const float base_spacing = scale.base_style().ItemSpacing.x;
    const auto  truncated    = [](float value)
    {
        return static_cast<float>(static_cast<int>(value));
    };

    scale.set_factor(1.5f);
    CHECK(scale.style().ItemSpacing.x == truncated(base_spacing * 1.5f));

    // Re-applying the same factor must not compound the scaling.
    scale.set_factor(1.5f);
    CHECK(scale.style().ItemSpacing.x == truncated(base_spacing * 1.5f));

    // Fractional factors are supported as-is, not rounded to an integer.
    scale.set_factor(1.75f);
    CHECK(scale.factor() == 1.75f);
    CHECK(scale.style().ItemSpacing.x == truncated(base_spacing * 1.75f));

    scale.set_factor(1.0f);
    CHECK(scale.style().ItemSpacing.x == base_spacing);
}

TEST_CASE("the embedded font loads into the atlas", "[ui]")
{
    ContextGuard guard;

    ImFont* font = slopkit::ui::load_embedded_font(slopkit::ui::kBaseFontSize);
    REQUIRE(font != nullptr);
}
