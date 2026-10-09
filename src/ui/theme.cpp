#include "ui/theme.hpp"

#include <QApplication>
#include <QGuiApplication>
#include <QStyleHints>

namespace slopkit::ui
{

    namespace
    {
        QColor rgb(float red, float green, float blue)
        {
            return QColor::fromRgbF(red, green, blue);
        }

        // The active theme the component library reads from; refreshed by
        // apply_theme().
        Theme g_active = dark_theme();

        void assign_group(QPalette& palette, QPalette::ColorGroup group, const Theme& theme)
        {
            palette.setColor(group, QPalette::Window, theme.background);
            palette.setColor(group, QPalette::WindowText, theme.text);
            palette.setColor(group, QPalette::Base, theme.surface);
            palette.setColor(group, QPalette::AlternateBase, theme.surface_hover);
            palette.setColor(group, QPalette::ToolTipBase, theme.surface);
            palette.setColor(group, QPalette::ToolTipText, theme.text);
            palette.setColor(group, QPalette::Text, theme.text);
            palette.setColor(group, QPalette::Button, theme.surface);
            palette.setColor(group, QPalette::ButtonText, theme.text);
            palette.setColor(group, QPalette::PlaceholderText, theme.text_muted);
            palette.setColor(group, QPalette::Light, theme.surface_hover);
            palette.setColor(group, QPalette::Midlight, theme.surface_hover);
            palette.setColor(group, QPalette::Mid, theme.border);
            palette.setColor(group, QPalette::Dark, theme.border);
            palette.setColor(group, QPalette::Shadow, theme.background);
            palette.setColor(group, QPalette::Highlight, theme.accent);
            palette.setColor(group, QPalette::HighlightedText, theme.on_accent);
            palette.setColor(group, QPalette::Link, theme.accent);
            palette.setColor(group, QPalette::LinkVisited, theme.accent_active);
        }
    } // namespace

    Theme dark_theme()
    {
        Theme theme;
        theme.background       = rgb(0.09f, 0.10f, 0.12f);
        theme.surface          = rgb(0.13f, 0.14f, 0.18f);
        theme.surface_hover    = rgb(0.18f, 0.20f, 0.25f);
        theme.text             = rgb(0.92f, 0.93f, 0.95f);
        theme.text_muted       = rgb(0.60f, 0.62f, 0.68f);
        theme.accent           = rgb(0.30f, 0.58f, 0.95f);
        theme.accent_hover     = rgb(0.38f, 0.66f, 0.99f);
        theme.accent_active    = rgb(0.24f, 0.49f, 0.84f);
        theme.on_accent        = rgb(1.00f, 1.00f, 1.00f);
        theme.border           = rgb(0.24f, 0.26f, 0.32f);
        theme.success          = rgb(0.42f, 0.80f, 0.50f);
        theme.warning          = rgb(0.94f, 0.74f, 0.32f);
        theme.error            = rgb(0.92f, 0.44f, 0.44f);
        theme.syntax_register  = rgb(0.46f, 0.74f, 0.98f);
        theme.syntax_immediate = rgb(0.86f, 0.63f, 0.95f);
        theme.syntax_module    = rgb(0.52f, 0.83f, 0.60f);
        theme.syntax_keyword   = rgb(0.94f, 0.58f, 0.42f);
        theme.syntax_string    = rgb(0.85f, 0.82f, 0.55f);
        theme.syntax_comment   = rgb(0.50f, 0.55f, 0.62f);
        theme.syntax_number    = rgb(0.90f, 0.65f, 0.98f);
        return theme;
    }

    Theme light_theme()
    {
        Theme theme;
        theme.background       = rgb(0.97f, 0.97f, 0.98f);
        theme.surface          = rgb(1.00f, 1.00f, 1.00f);
        theme.surface_hover    = rgb(0.92f, 0.93f, 0.95f);
        theme.text             = rgb(0.12f, 0.13f, 0.16f);
        theme.text_muted       = rgb(0.42f, 0.44f, 0.50f);
        theme.accent           = rgb(0.15f, 0.42f, 0.86f);
        theme.accent_hover     = rgb(0.20f, 0.49f, 0.93f);
        theme.accent_active    = rgb(0.11f, 0.34f, 0.72f);
        theme.on_accent        = rgb(1.00f, 1.00f, 1.00f);
        theme.border           = rgb(0.80f, 0.82f, 0.86f);
        theme.success          = rgb(0.20f, 0.60f, 0.30f);
        theme.warning          = rgb(0.72f, 0.50f, 0.10f);
        theme.error            = rgb(0.78f, 0.22f, 0.22f);
        theme.syntax_register  = rgb(0.13f, 0.35f, 0.78f);
        theme.syntax_immediate = rgb(0.55f, 0.18f, 0.62f);
        theme.syntax_module    = rgb(0.10f, 0.45f, 0.25f);
        theme.syntax_keyword   = rgb(0.70f, 0.20f, 0.10f);
        theme.syntax_string    = rgb(0.35f, 0.35f, 0.05f);
        theme.syntax_comment   = rgb(0.45f, 0.48f, 0.55f);
        theme.syntax_number    = rgb(0.45f, 0.15f, 0.60f);
        return theme;
    }

    QPalette make_palette(const Theme& theme)
    {
        QPalette palette;
        assign_group(palette, QPalette::Active, theme);
        assign_group(palette, QPalette::Inactive, theme);

        // Disabled controls keep their surfaces but fade their text.
        assign_group(palette, QPalette::Disabled, theme);
        palette.setColor(QPalette::Disabled, QPalette::WindowText, theme.text_muted);
        palette.setColor(QPalette::Disabled, QPalette::Text, theme.text_muted);
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, theme.text_muted);
        palette.setColor(QPalette::Disabled, QPalette::HighlightedText, theme.text_muted);
        palette.setColor(QPalette::Disabled, QPalette::Highlight, theme.surface_hover);
        palette.setColor(QPalette::Disabled, QPalette::Link, theme.text_muted);
        return palette;
    }

    void apply_theme(const Theme& theme)
    {
        g_active = theme;

        if (QApplication::instance() == nullptr)
        {
            return;
        }

        // Tell the platform which colour scheme is in use so the native parts
        // (file dialogs, window decorations) follow, then install our palette on
        // top of it.
        if (QStyleHints* hints = QGuiApplication::styleHints())
        {
            const bool dark = theme.background.lightness() < 128;
            hints->setColorScheme(dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
        }
        QApplication::setPalette(make_palette(theme));
    }

    const Theme& active_theme()
    {
        return g_active;
    }

} // namespace slopkit::ui
