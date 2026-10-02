#pragma once

#include <QColor>
#include <QPalette>

namespace slopkit::ui
{

    // Semantic colour roles. Every role is defined by both themes, and widget
    // code must read colours from the active theme rather than hard-coding them.
    struct Theme
    {
        QColor background;
        QColor surface;
        QColor surface_hover;
        QColor text;
        QColor text_muted;
        QColor accent;
        QColor accent_hover;
        QColor accent_active;
        QColor on_accent;
        QColor border;
        QColor success;
        QColor warning;
        QColor error;
    };

    [[nodiscard]] Theme dark_theme();
    [[nodiscard]] Theme light_theme();

    // Maps the semantic roles onto the palette roles Qt's Fusion style paints
    // from, for every colour group.
    [[nodiscard]] QPalette make_palette(const Theme& theme);

    // Makes `theme` the active theme the component library reads from and
    // installs its palette application-wide; requires a QApplication.
    void apply_theme(const Theme& theme);

    [[nodiscard]] const Theme& active_theme();

} // namespace slopkit::ui
