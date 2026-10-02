#include "ui/scale.hpp"

namespace slopkit::ui
{

    ImGuiStyle make_base_style()
    {
        ImGuiStyle style;

        // Sizes are logical units at scale 1.0.
        style.WindowPadding    = ImVec2(12.0f, 12.0f);
        style.FramePadding     = ImVec2(10.0f, 6.0f);
        style.CellPadding      = ImVec2(8.0f, 5.0f);
        style.ItemSpacing      = ImVec2(10.0f, 8.0f);
        style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
        style.IndentSpacing    = 18.0f;
        style.ScrollbarSize    = 14.0f;
        style.GrabMinSize      = 12.0f;

        style.WindowRounding    = 8.0f;
        style.ChildRounding     = 8.0f;
        style.FrameRounding     = 6.0f;
        style.PopupRounding     = 8.0f;
        style.ScrollbarRounding = 8.0f;
        style.GrabRounding      = 6.0f;
        style.TabRounding       = 6.0f;

        style.WindowBorderSize = 0.0f;
        style.ChildBorderSize  = 1.0f;
        style.FrameBorderSize  = 0.0f;
        style.PopupBorderSize  = 1.0f;

        style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
        style.ButtonTextAlign  = ImVec2(0.5f, 0.5f);

        return style;
    }

    Scale::Scale() : base_(make_base_style()), active_(base_), factor_(1.0f) {}

    float Scale::factor() const noexcept
    {
        return factor_;
    }

    void Scale::set_factor(float factor)
    {
        factor_ = factor;
        active_ = base_;
        active_.ScaleAllSizes(factor_);
    }

    const ImGuiStyle& Scale::base_style() const noexcept
    {
        return base_;
    }

    ImGuiStyle& Scale::style() noexcept
    {
        return active_;
    }

    const ImGuiStyle& Scale::style() const noexcept
    {
        return active_;
    }

} // namespace slopkit::ui
