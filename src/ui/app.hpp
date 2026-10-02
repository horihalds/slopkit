#pragma once

#include <memory>
#include <optional>

#include "plugin/plugin_host.hpp"
#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/plugin_access.hpp"
#include "table/address_table.hpp"
#include "ui/app_window.hpp"
#include "ui/dialogs/add_address.hpp"
#include "ui/dialogs/memory_viewer.hpp"
#include "ui/dialogs/process_list.hpp"
#include "ui/dialogs/settings.hpp"
#include "ui/panels/address_list_panel.hpp"
#include "ui/panels/found_list_panel.hpp"
#include "ui/panels/scanner_panel.hpp"
#include "ui/panels/top_bar.hpp"
#include "ui/scale.hpp"
#include "ui/theme.hpp"

namespace slopkit::ui
{

    // Owns the plugin host, the window, the active theme and the DPI scale, and
    // lays the main window out as a top bar, a split middle scan area and a
    // full-width address list below.
    class App
    {
    public:
        App()                      = default;
        ~App()                     = default;
        App(const App&)            = delete;
        App& operator=(const App&) = delete;

        int run();

    private:
        void apply_active_style();
        void draw_ui();

        plugin::PluginHost         host_;
        process::PluginAccess      access_ {host_};
        process::AccessWorker      access_worker_ {access_};
        process::AttachedTarget    target_;
        dialogs::ProcessList       process_list_ {access_worker_, target_};
        std::unique_ptr<AppWindow> window_;
        Theme                      theme_ = dark_theme();
        Scale                      scale_;

        table::AddressTable      address_table_;
        panels::TopBar           top_bar_;
        panels::ScannerPanel     scanner_ {access_worker_, target_};
        panels::FoundListPanel   found_list_ {scanner_.engine(), address_table_};
        panels::AddressListPanel address_list_ {address_table_, access_worker_, target_};
        dialogs::Settings        settings_ {host_, scanner_.engine()};
        dialogs::AddAddress      add_address_ {address_table_};
        dialogs::MemoryViewer    memory_viewer_ {access_worker_, target_};

        // In-flight freeze job, if any; one at a time.
        std::optional<process::JobId> freeze_pending_;

        // Fraction of the vertical space given to the middle scan zone.
        float middle_ratio_ {0.62f};
        // Fraction of the middle zone's width given to the Found list.
        float found_ratio_ {0.5f};

        bool show_process_list_ {false};
        bool show_settings_ {false};
        bool show_add_address_ {false};
        bool show_memory_viewer_ {false};
        bool should_quit_ {false};
        bool theme_is_dark_ {true};
    };

} // namespace slopkit::ui
