#pragma once

#include <imgui.h>

namespace slopkit::ui
{

    // The unscaled design baseline: rounding, padding and spacing expressed in
    // logical units at scale 1.0.
    [[nodiscard]] ImGuiStyle make_base_style();

    // Holds an unscaled base style and derives the active style from it with
    // ScaleAllSizes(). Re-scaling always starts from the base, so repeated
    // scale changes never compound.
    class Scale
    {
    public:
        Scale();

        [[nodiscard]] float factor() const noexcept;

        // Rebuilds the active style at the given factor. Fractional factors are
        // supported; the value is never rounded to an integer.
        void set_factor(float factor);

        [[nodiscard]] const ImGuiStyle& base_style() const noexcept;
        [[nodiscard]] ImGuiStyle&       style() noexcept;
        [[nodiscard]] const ImGuiStyle& style() const noexcept;

    private:
        ImGuiStyle base_;
        ImGuiStyle active_;
        float      factor_ {1.0f};
    };

} // namespace slopkit::ui
