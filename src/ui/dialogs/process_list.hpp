#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/types.hpp"

namespace slopkit::ui::dialogs
{

    // The Process List dialog. It lists processes split into Applications
    // (desktop-entry-backed) and Processes, probes the selection and writes the
    // app-wide AttachedTarget on attach/detach.
    class ProcessList
    {
    public:
        ProcessList(process::AccessWorker& worker, process::AttachedTarget& target);

        void draw(bool& open);

    private:
        void refresh();
        void probe_selection();
        void attach_selected();
        void detach();
        void maybe_auto_refresh();
        void request_application_index();
        void draw_body(bool applications_only);
        void draw_detail(const process::ProcessInfo& info);

        [[nodiscard]] const process::ProcessInfo* selected() const;
        [[nodiscard]] std::string                 chosen_plugin(const process::ProcessInfo& info) const;
        [[nodiscard]] bool                        is_application(const process::ProcessInfo& info) const;
        void                                      set_status(std::string message, bool is_error);

        process::AccessWorker&   worker_;
        process::AttachedTarget& target_;

        std::vector<process::ProcessInfo> processes_;
        std::vector<int>                  visible_;
        std::vector<std::string>          plugin_ids_;
        std::vector<std::string>          application_executables_;
        bool                              index_built_ {false};

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

        std::string status_;
        bool        status_is_error_ {false};

        std::optional<process::JobId> attach_pending_;
        std::optional<process::JobId> detach_pending_;
        std::optional<process::JobId> list_pending_;
        std::optional<process::JobId> probe_pending_;
        std::optional<process::JobId> index_pending_;
        int                           probe_pending_pid_ {-1};

        bool   auto_refresh_ {true};
        double next_refresh_time_ {0.0};
    };

} // namespace slopkit::ui::dialogs
