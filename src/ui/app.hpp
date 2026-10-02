#pragma once

#include <memory>

#include "plugin/plugin_host.hpp"
#include "process/plugin_access.hpp"
#include "ui/app_window.hpp"
#include "ui/panels/process_picker.hpp"
#include "ui/scale.hpp"
#include "ui/theme.hpp"

namespace slopkit::ui
{

    // Owns the plugin host, the window, the active theme and the DPI scale, and
    // drives the tool navigation.
    class App
    {
    public:
        App()                      = default;
        ~App()                     = default;
        App(const App&)            = delete;
        App& operator=(const App&) = delete;

        int run();

    private:
        enum class Tool
        {
            process_picker,
            scanner,
            browser,
            debugger,
        };

        void apply_active_style();
        void draw_ui();
        void draw_nav_entry(const char* label, Tool tool);

        plugin::PluginHost         host_;
        process::PluginAccess      access_ {host_};
        panels::ProcessPicker      picker_ {access_};
        std::unique_ptr<AppWindow> window_;
        Theme                      theme_ = dark_theme();
        Scale                      scale_;
        Tool                       selected_ {Tool::process_picker};
    };

} // namespace slopkit::ui
