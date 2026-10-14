#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <QDialog>
#include <QString>

#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/types.hpp"
#include "reclass/node.hpp"
#include "reclass/project.hpp"
#include "ui/live_values.hpp"

class QAction;
class QCloseEvent;
class QLabel;
class QListWidget;
class QMenu;
class QModelIndex;
class QPoint;
class QPushButton;
class QToolButton;
class QTreeView;

namespace slopkit::ui
{
    class SettingsController;
}

namespace slopkit::ui::models
{
    struct ReclassRow;
    class ReclassProxyModel;
    class ReclassStructureModel;
    class ReclassValueModel;
}

namespace slopkit::ui::widgets
{
    class StatusLabel;
}

namespace slopkit::ui::dialogs
{
    // The ReClass window: the project's classes on the left, the selected
    // class's node grid on the right, refreshed on the shared live cadence.
    class ReClassDialog : public QDialog, public LiveSurface
    {
        Q_OBJECT

    public:
        ReClassDialog(reclass::Project&        project,
                      process::AccessWorker&   worker,
                      process::AttachedTarget& target,
                      QWidget*                 parent = nullptr);

        // The memory map the classes' expressions are resolved against, and the
        // spans the dissect command tests a pointer value for.
        void set_modules(std::vector<process::ModuleInfo> modules);

        [[nodiscard]] models::ReclassStructureModel* structure() const noexcept;
        [[nodiscard]] models::ReclassProxyModel*     proxy() const noexcept;
        [[nodiscard]] QListWidget*                   class_list() const noexcept;
        [[nodiscard]] QTreeView*                     grid() const noexcept;
        [[nodiscard]] QLabel*                        header_label() const noexcept;
        [[nodiscard]] widgets::StatusLabel*          status_label() const noexcept;
        [[nodiscard]] QPushButton*                   add_class_button() const noexcept;
        [[nodiscard]] QPushButton*                   delete_class_button() const noexcept;
        [[nodiscard]] QToolButton*                   add_node_button() const noexcept;
        [[nodiscard]] QPushButton*                   delete_node_button() const noexcept;

        [[nodiscard]] std::vector<LiveRequest> next_live_request() override;
        void                                   apply_live_readings(std::span<const LiveReading> readings) override;

        // The window's commands, shared by the toolbar, the menus and the tests.
        void add_class();
        void rename_class();
        void set_class_address();
        void delete_class();

        // The class edits without their prompts, so a test can drive them
        // without opening a modal box; each reports on the status line.
        [[nodiscard]] bool create_class(const QString& name, const QString& expression);
        [[nodiscard]] bool rename_selected_class(const QString& name);
        [[nodiscard]] bool set_selected_class_address(const QString& address);

        void insert_node(reclass::NodeKind kind);
        void change_node_kind(reclass::NodeKind kind);

        // Dissect guesses a better kind for the selected hex nodes from their
        // live bytes; Follow Pointer sets the class a pointer node refers to and
        // inlines its fields.
        void dissect();
        void follow_pointer();
        void set_followed_class(const QString& class_name);

        [[nodiscard]] QPushButton* dissect_button() const noexcept;
        [[nodiscard]] QPushButton* follow_button() const noexcept;

        void add_bytes();
        void edit_name();
        void edit_comment();
        void set_node_hex(bool hex);
        void copy_name();
        void remove_selected_nodes();

        [[nodiscard]] std::optional<std::size_t> current_node_id() const;
        [[nodiscard]] std::vector<std::size_t>   selected_node_ids() const;

        // Re-reads the project after the table that carries it was replaced.
        void refresh_project();

    protected:
        void closeEvent(QCloseEvent* event) override;

    private:
        void build_layout();
        void connect_signals();

        QWidget* build_class_pane();
        QWidget* build_grid_pane();

        void                 populate_node_kind_menu(QMenu& menu, bool change);
        [[nodiscard]] QMenu* build_add_node_menu();

        void show_class_menu(const QPoint& position);
        void show_row_menu(const QPoint& position);

        void on_class_selected(int row);
        void on_grid_expanded(const QModelIndex& index);
        void on_grid_collapsed(const QModelIndex& index);

        // The project or the selected class changed: the list, the grid and the
        // remembered cursor all follow.
        void refresh_all();
        void refresh_class_list();
        void refresh_header();
        void refresh_class_tooltips();
        void sync_expansion();
        void update_actions();

        void report(const QString& message, bool error = false);

        [[nodiscard]] bool    selected_class_exists() const;
        [[nodiscard]] QString validate_expression(const QString& text) const;
        [[nodiscard]] QString class_name_error(const QString& name, const reclass::Class* current) const;

        [[nodiscard]] std::vector<const models::ReclassRow*> selected_rows() const;
        [[nodiscard]] reclass::Node*                         node_at(const models::ReclassRow* row) const;

        // One hex node the dissect command is guessing a kind for.
        struct DissectTarget
        {
            std::size_t            class_index {};
            std::size_t            node_index {};
            reclass::NodeKind      kind {reclass::NodeKind::hex8};
            std::size_t            offset {};
            std::uint64_t          address {};
            std::vector<std::byte> bytes;
        };

        // Replaces every target the ladder found a better kind for.
        void apply_dissect(const std::vector<std::uint64_t>& readable);

        reclass::Project&        project_;
        process::AccessWorker*   worker_ {};
        process::AttachedTarget* target_ {};

        models::ReclassStructureModel* structure_ {};
        models::ReclassValueModel*     values_ {};
        models::ReclassProxyModel*     proxy_ {};

        QListWidget*          class_list_ {};
        QPushButton*          add_class_ {};
        QPushButton*          delete_class_ {};
        QLabel*               header_ {};
        QTreeView*            grid_ {};
        QToolButton*          add_node_ {};
        QPushButton*          delete_node_ {};
        QPushButton*          dissect_ {};
        QPushButton*          follow_ {};
        widgets::StatusLabel* status_ {};
        QPushButton*          close_ {};
        QAction*              delete_action_ {};

        std::optional<process::JobId> dissect_job_;
        std::vector<DissectTarget>    dissect_targets_;

        // Guards the view's expansion signals against the model reset an inline
        // or an outline performs.
        bool syncing_ {};
    };

}
