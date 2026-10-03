#include "ui/app.hpp"

#include <QApplication>
#include <QGuiApplication>
#include <QStyleFactory>

#include "app/cli.hpp"
#include "core/log.hpp"
#include "ui/fonts.hpp"
#include "ui/theme.hpp"

namespace slopkit::ui
{

    int App::run(int argc, char** argv)
    {
        QApplication application(argc, argv);
        QApplication::setApplicationName(QStringLiteral("slopkit"));
        QApplication::setApplicationDisplayName(QStringLiteral("slopkit"));
        // Keeps the Wayland app-id in step with `StartupWMClass=slopkit` in
        // assets/slopkit.desktop.in.
        QGuiApplication::setDesktopFileName(QStringLiteral("slopkit"));

        // A single, predictable style makes the palette-only theming behave the
        // same on every desktop.
        application.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

        if (!register_embedded_fonts())
        {
            log::warning("ui", "the embedded fonts could not be loaded");
        }
        apply_theme(settings_.values().dark_theme ? dark_theme() : light_theme());

        host_.discover(app::plugin_search_directories());

        window_ = std::make_unique<MainWindow>(access_worker_, target_, host_, settings_);
        window_->show();

        // The worker notifies from its own thread; the queued wake-up drains the
        // completions here, on the UI thread.
        access_worker_.set_completion_hook(
            [this]
            {
                notifier_.post();
            });
        QObject::connect(&notifier_,
                         &CompletionNotifier::completionsAvailable,
                         &notifier_,
                         [this]
                         {
                             access_worker_.drain();
                         });

        const int exit_code = QApplication::exec();

        // The window must be gone before the worker and its hook are torn down.
        access_worker_.set_completion_hook(nullptr);
        window_.reset();
        return exit_code;
    }

} // namespace slopkit::ui
