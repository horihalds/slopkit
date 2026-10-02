#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "plugin/plugin_host.hpp"
#include "scan/engine.hpp"

namespace slopkit::ui::dialogs
{

    // The Settings dialog: a category list on the left and the selected
    // category on the right. Nothing is persisted between runs.
    class Settings
    {
    public:
        Settings(plugin::PluginHost& host, scan::ScanEngine& engine);

        // The changes the user asked for, applied by the App.
        struct Result
        {
            // The requested theme, so the App can re-apply the style.
            std::optional<bool>          dark_theme;
            // The new fast-scan alignment, forwarded to the ScannerPanel.
            std::optional<std::uint64_t> default_alignment;
        };

        Result draw(bool& open, bool dark_theme);

        // Selects the About category; used by Help > About.
        void select_about();

    private:
        void draw_appearance(Result& result, bool dark_theme);
        void draw_scanning(Result& result);
        void draw_plugins();
        void draw_about();
        void apply_scanning(Result& result);
        void set_status(std::string message, bool is_error);

        plugin::PluginHost&  host_;
        scan::ScanEngine&    engine_;
        int                  category_ {0};
        std::array<char, 16> alignment_ {"4"};
        std::array<char, 32> result_cap_ {"1000000"};
        std::string          status_;
        bool                 status_is_error_ {false};
    };

} // namespace slopkit::ui::dialogs
