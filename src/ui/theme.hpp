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
        // The disassembly listing's machine-text roles: a register name, an
        // immediate value (or displacement) and a module name.
        QColor syntax_register;
        QColor syntax_immediate;
        QColor syntax_module;
        // The script editor's Lua source roles: a keyword, a string, a comment
        // and a number literal.
        QColor syntax_keyword;
        QColor syntax_string;
        QColor syntax_comment;
        QColor syntax_number;
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
