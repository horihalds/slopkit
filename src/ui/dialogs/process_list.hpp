#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/types.hpp"
#include "ui/models/process_list_model.hpp"

#include <QDialog>
#include <QString>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTabBar;
class QTableView;
class QTimer;
class QVBoxLayout;

namespace slopkit::ui::widgets
{
    class StatusLabel;
    enum class StatusKind;
} // namespace slopkit::ui::widgets

namespace slopkit::ui::dialogs
{

    // The Process List dialog: the Applications / Processes views, the detail
    // pane, attach / detach and the auto-refresh. It writes the app-wide
    // AttachedTarget and never touches a session itself.
    class ProcessListDialog : public QDialog
    {
        Q_OBJECT

    public:
        ProcessListDialog(process::AccessWorker& worker, process::AttachedTarget& target, QWidget* parent = nullptr);

        // Submits a process list job (no-op while one is pending).
        void refresh();

        // Builds the desktop-entry index used by the Applications view.
        void request_application_index();

    signals:
        // Emitted after an attach or detach changed AttachedTarget.
        void targetChanged();

    protected:
        void showEvent(QShowEvent* event) override;
        bool eventFilter(QObject* watched, QEvent* event) override;

    private:
        void build_layout();
        void rebuild_plugin_filter();
        void after_model_change();
        void update_claimants(const process::ProcessInfo& info);
        void probe_selection();
        void attach_selected();
        void detach_selected();
        void update_buttons();
        void update_detail();
        void restore_selection();

        // Moves the highlighted row by `delta` (wrapping at both ends); the
        // table's current-row signal drives the probe, the details and the buttons.
        void step_selection(int delta);

        [[nodiscard]] const process::ProcessInfo* selected() const;
        [[nodiscard]] std::string                 chosen_plugin(const process::ProcessInfo& info) const;
        void                                      set_message(std::string message, widgets::StatusKind kind);
        void                                      clear_message();

        process::AccessWorker&   worker_;
        process::AttachedTarget& target_;

        models::ProcessListModel* model_ {};
        QTableView*               table_view_ {};
        QTabBar*                  view_tabs_ {};
        QLineEdit*                search_edit_ {};
        QComboBox*                plugin_combo_ {};

        QLabel*               detail_pid_ {};
        QLabel*               detail_name_ {};
        QLabel*               detail_executable_ {};
        QLabel*               detail_plugin_ {};
        QWidget*              detail_body_ {};
        QLabel*               detail_hint_ {};
        QWidget*              claimants_area_ {};
        QVBoxLayout*          claimants_layout_ {};
        QLabel*               detail_access_ {};
        QLabel*               detail_modules_ {};
        QLabel*               detail_threads_ {};
        QLabel*               detail_memory_ {};
        QLabel*               detail_inspecting_ {};
        QPushButton*          attach_button_ {};
        widgets::StatusLabel* attached_label_ {};
        widgets::StatusLabel* message_label_ {};

        QTimer* auto_refresh_timer_ {};

        bool index_built_ {false};

        std::vector<std::string> plugin_ids_;

        // Keeps the row selection (by pid) across the model's resets.
        process::ProcessId selected_pid_ {0};
        bool               restoring_ {false};

        // Cached detail for the selected process.
        int                                 detail_pid_value_ {-1};
        process::AccessMethod               detail_methods_ {process::AccessMethod::none};
        std::size_t                         detail_module_count_ {0};
        std::size_t                         detail_thread_count_ {0};
        std::optional<process::AccessError> detail_read_error_;
        // True once a probe reached the memory-read check, so the verdict line is
        // only shown when a verdict actually exists.
        bool                                detail_read_checked_ {false};

        std::string selected_plugin_;
        std::string message_;

        std::optional<process::JobId> attach_pending_;
        std::optional<process::JobId> detach_pending_;
        std::optional<process::JobId> list_pending_;
        std::optional<process::JobId> probe_pending_;
        std::optional<process::JobId> index_pending_;
        int                           probe_pending_pid_ {-1};
    };

} // namespace slopkit::ui::dialogs
