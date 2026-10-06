#include <iostream>
#include <string_view>

#include <QApplication>
#include <QGuiApplication>
#include <QString>
#include <QStyleFactory>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "sandbox/sandbox_values.hpp"
#include "sandbox/sandbox_window.hpp"
#include "ui/fonts.hpp"
#include "ui/platform.hpp"
#include "ui/settings.hpp"
#include "ui/theme.hpp"

int main(int argc, char** argv)
{
    // stderr only: the sandbox must never interleave with slopkit's rolling log
    // file under ~/.local/state/slopkit.
    slopkit::log::Logger::instance().add_sink(slopkit::log::stderr_sink());

    for (int index = 1; index < argc; ++index)
    {
        if (std::string_view(argv[index]) == "--print-layout")
        {
            // Headless: print the value layout and exit before any widget (or
            // even a QApplication) is built, so no display is required.
            slopkit::sandbox::print_layout(std::cout);
            return 0;
        }
    }

    // Same platform choice as slopkit, so the two windows never disagree and
    // the sandbox follows slopkit back onto XWayland.
    slopkit::ui::prefer_platform_with_remembered_positions();

    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("slopkit-sandbox"));
    QApplication::setApplicationDisplayName(QStringLiteral("slopkit sandbox"));
    // No `.desktop` entry ships for the sandbox, so setDesktopFileName() is
    // deliberately left unset.

    // A single, predictable style makes the palette-only theming behave the same
    // on every desktop.
    application.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    if (!slopkit::ui::register_embedded_fonts())
    {
        slopkit::log::warning(slopkit::log::category::ui, "the embedded fonts could not be loaded");
    }

    // Read the persisted theme once so the sandbox matches whatever the user
    // chose in slopkit, without a second settings store of its own.
    const slopkit::ui::SettingsController settings;
    slopkit::ui::apply_theme(settings.values().dark_theme ? slopkit::ui::dark_theme() : slopkit::ui::light_theme());

    slopkit::log::info(slopkit::log::category::app, "slopkit sandbox starting");

    slopkit::sandbox::SandboxWindow window;
    window.show();

    return QApplication::exec();
}
