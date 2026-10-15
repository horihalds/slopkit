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
        QColor datatype_signed;
        QColor datatype_unsigned;
        QColor datatype_bits;
        QColor datatype_float;
        QColor datatype_vector;
        QColor datatype_text;
        QColor datatype_class;
        QColor datatype_readout;
        QColor grid_offset;
        QColor grid_address;
        QColor grid_ascii;
        QColor grid_comment;
        QColor grid_pointer;
    };

    [[nodiscard]] Theme dark_theme();
    [[nodiscard]] Theme light_theme();

    [[nodiscard]] QPalette make_palette(const Theme& theme);

    void apply_theme(const Theme& theme);

    [[nodiscard]] const Theme& active_theme();

}
