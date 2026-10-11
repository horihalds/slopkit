#pragma once

#include <QColor>
#include <QPalette>

namespace slopkit::ui
{
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
        QColor syntax_register;
        QColor syntax_immediate;
        QColor syntax_module;
        QColor syntax_keyword;
        QColor syntax_string;
        QColor syntax_comment;
        QColor syntax_number;
    };

    [[nodiscard]] Theme dark_theme();
    [[nodiscard]] Theme light_theme();

    [[nodiscard]] QPalette make_palette(const Theme& theme);

    void apply_theme(const Theme& theme);

    [[nodiscard]] const Theme& active_theme();

}
