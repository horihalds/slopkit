#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "process/access.hpp"
#include "process/types.hpp"

namespace slopkit::ui::panels
{

    // The process picker: the first real tool. It lists processes, inspects the
    // selection and attaches/detaches, and talks to the plugin layer only
    // through the process::ProcessAccess seam.
    class ProcessPicker
    {
    public:
        explicit ProcessPicker(process::ProcessAccess& access);

        void draw();

    private:
        void refresh();
        void probe_selection();
        void attach_selected();
        void detach();
        void maybe_auto_refresh();

        [[nodiscard]] const process::ProcessInfo* selected() const;
        [[nodiscard]] std::string                 chosen_plugin(const process::ProcessInfo& info) const;
        void                                      set_status(std::string message, bool is_error);

        process::ProcessAccess& access_;

        std::vector<process::ProcessInfo> processes_;
        std::vector<int>                  visible_;
        std::vector<std::string>          plugin_ids_;

        std::string search_;
        std::string plugin_filter_;
        int         selected_index_ {-1};
        std::string selected_plugin_;
        bool        probe_needed_ {true};

        // Cached detail for the selected process.
        int                   detail_pid_ {-1};
        process::AccessMethod detail_methods_ {process::AccessMethod::none};
        std::size_t           detail_module_count_ {0};
        std::size_t           detail_thread_count_ {0};
        std::string           detail_error_;

        process::Session session_;
        std::string      status_;
        bool             status_is_error_ {false};

        bool   auto_refresh_ {true};
        double next_refresh_time_ {0.0};
    };

} // namespace slopkit::ui::panels
