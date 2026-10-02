#pragma once

#include <memory>

#include "plugin/plugin_host.hpp"
#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/plugin_access.hpp"
#include "ui/completion_notifier.hpp"
#include "ui/main_window.hpp"

namespace slopkit::ui
{

    // Owns the plugin host, the access worker and the main window, and runs the
    // Qt event loop. Worker completions arrive as a queued wake-up and are
    // applied on the UI thread.
    class App
    {
    public:
        App()                      = default;
        ~App()                     = default;
        App(const App&)            = delete;
        App& operator=(const App&) = delete;

        // Builds the QApplication, applies the application identity, discovers
        // the plugins, shows the main window and runs the event loop. Returns
        // the process exit code. Must be called at most once per process.
        int run(int argc, char** argv);

    private:
        plugin::PluginHost      host_;
        process::PluginAccess   access_ {host_};
        // Declared before the worker so it outlives the worker thread that calls
        // its post().
        CompletionNotifier      notifier_;
        process::AccessWorker   access_worker_ {access_};
        process::AttachedTarget target_;

        std::unique_ptr<MainWindow> window_;
    };

} // namespace slopkit::ui
