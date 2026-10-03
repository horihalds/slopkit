#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/types.hpp"

#include <QAbstractTableModel>
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
} // namespace slopkit::ui::widgets

namespace slopkit::ui::dialogs
{

    // PID / Name rows for one tab, search text and plugin filter of the
    // Process List dialog.
    class ProcessListModel : public QAbstractTableModel
    {
        Q_OBJECT

    public:
        enum Column
        {
            pid,
            name,
            column_count,
        };

        explicit ProcessListModel(QObject* parent = nullptr);

        [[nodiscard]] int      rowCount(const QModelIndex& parent = QModelIndex()) const override;
        [[nodiscard]] int      columnCount(const QModelIndex& parent = QModelIndex()) const override;
        [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
        [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
        void                   sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

        void set_processes(std::vector<process::ProcessInfo> processes);
        void set_application_index(std::vector<std::string> executables);
        void set_applications_only(bool applications_only);
        void set_search(const QString& text);
        void set_plugin_filter(const QString& plugin_id);

        [[nodiscard]] const process::ProcessInfo* process_at(int row) const;
        [[nodiscard]] int                         row_for_pid(process::ProcessId pid) const;

    private:
        void rebuild();
        void apply_sort();

        std::vector<process::ProcessInfo> processes_;
        std::vector<std::string>          application_executables_;
        std::vector<int>                  visible_;

        bool          applications_only_ {false};
        QString       search_;
        QString       plugin_filter_;
        int           sort_column_ {pid};
        Qt::SortOrder sort_order_ {Qt::AscendingOrder};
    };

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
        void update_status();
        void restore_selection();

        [[nodiscard]] const process::ProcessInfo* selected() const;
        [[nodiscard]] std::string                 chosen_plugin(const process::ProcessInfo& info) const;
        void                                      set_status(std::string message, bool is_error);

        process::AccessWorker&   worker_;
        process::AttachedTarget& target_;

        ProcessListModel* model_ {};
        QTableView*       table_view_ {};
        QTabBar*          view_tabs_ {};
        QLineEdit*        search_edit_ {};
        QComboBox*        plugin_combo_ {};

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
        QLabel*               detail_inspecting_ {};
        widgets::StatusLabel* detail_error_ {};
        QPushButton*          attach_button_ {};
        widgets::StatusLabel* attached_label_ {};
        widgets::StatusLabel* status_ {};

        QTimer* auto_refresh_timer_ {};

        bool index_built_ {false};

        std::vector<std::string> plugin_ids_;

        // Keeps the row selection (by pid) across the model's resets.
        process::ProcessId selected_pid_ {0};
        bool               restoring_ {false};

        // Cached detail for the selected process.
        int                   detail_pid_value_ {-1};
        process::AccessMethod detail_methods_ {process::AccessMethod::none};
        std::size_t           detail_module_count_ {0};
        std::size_t           detail_thread_count_ {0};
        std::string           detail_error_message_;

        std::string selected_plugin_;
        std::string status_text_;
        bool        status_is_error_ {false};

        std::optional<process::JobId> attach_pending_;
        std::optional<process::JobId> detach_pending_;
        std::optional<process::JobId> list_pending_;
        std::optional<process::JobId> probe_pending_;
        std::optional<process::JobId> index_pending_;
        int                           probe_pending_pid_ {-1};
    };

} // namespace slopkit::ui::dialogs
