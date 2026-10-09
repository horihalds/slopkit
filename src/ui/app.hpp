#pragma once

#include <memory>

#include <QString>

#include "app/instance_server.hpp"
#include "debug/controller.hpp"
#include "debug/plugin_backend.hpp"
#include "plugin/plugin_host.hpp"
#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/plugin_access.hpp"
#include "script/symbols.hpp"
#include "ui/completion_notifier.hpp"
#include "ui/main_window.hpp"
#include "ui/settings.hpp"

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
        // the process exit code. Must be called at most once per process. A
        // non-empty `initial_table_path` opens that table instead of the
        // remembered auto-load one.
        int run(int argc, char** argv, const QString& initial_table_path = QString());

    private:
        // The process-wide symbol registry scripts publish into. Declared first
        // so it outlives the worker, the controller and the window that hold a
        // reference to it.
        script::SymbolTable symbols_;

        plugin::PluginHost    host_;
        process::PluginAccess access_ {host_};

        // The debug session: a plugin backend over the same host and the
        // controller that owns its worker. Declared before the window so the
        // window and its panes are destroyed first.
        debug::PluginBackend debug_backend_ {host_};
        debug::Controller    debug_ {debug_backend_, symbols_};

        // Declared before the workers so it outlives the threads that call its
        // post().
        CompletionNotifier      notifier_;
        process::AccessWorker   access_worker_ {access_, symbols_};
        process::AttachedTarget target_;
        SettingsController      settings_;

        std::unique_ptr<MainWindow> window_;

        // Declared after the window so it is reset before the window on
        // shutdown; the listener must not outlive the open flow it feeds.
        std::unique_ptr<app::InstanceServer> server_;
    };

} // namespace slopkit::ui
