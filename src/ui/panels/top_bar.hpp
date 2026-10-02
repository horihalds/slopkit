#pragma once

#include <string>

namespace slopkit::ui::panels
{

    // The top zone: the menu bar, the toolbar, the attached-process label and
    // the scan progress bar. It is pure presentation and owns no session state;
    // the actions the user requests are reported back to the App.
    class TopBar
    {
    public:
        struct Model
        {
            std::string process_label {"No Process Selected"};
            float       progress {0.0f};
            bool        scanning {false};
        };

        // The set of commands the user requested this frame.
        struct Actions
        {
            bool open_process {false};
            bool open_table {false};
            bool save_table {false};
            bool undo_scan {false};
            bool add_address {false};
            bool delete_selected {false};
            bool freeze_selected {false};
            bool settings {false};
            bool about {false};
            bool quit {false};
        };

        [[nodiscard]] Actions draw(const Model& model);
    };

} // namespace slopkit::ui::panels
