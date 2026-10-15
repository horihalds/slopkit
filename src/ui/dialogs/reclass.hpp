#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <QDialog>
#include <QString>

#include "debug/breakpoints.hpp"
#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/types.hpp"
#include "reclass/node.hpp"
#include "reclass/nodes/node.hpp"
#include "reclass/project.hpp"
#include "ui/live_values.hpp"

class QAction;
class QCloseEvent;
class QLabel;
class QMenu;
class QMenuBar;
class QModelIndex;
class QPoint;
class QPushButton;
class QStatusBar;
class QToolBar;
class QToolButton;

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

namespace slopkit::ui::components
{
    class ProjectView;
    class ReClassView;
}

namespace slopkit::ui::dialogs
{
    // The ReClass window, shaped after the reference's MainForm: a menu strip and
    // a toolbar over a splitter that puts the class/enum project tree beside the
    // selected class's node grid, with a status strip below. The grid is
    // refreshed on the shared live cadence.
    class ReClassDialog : public QDialog, public LiveSurface
    {
        Q_OBJECT

    public:
        ReClassDialog(reclass::Project&        project,
                      process::AccessWorker&   worker,
                      process::AttachedTarget& target,
                      QWidget*                 parent = nullptr);

        // The memory map the classes' expressions are resolved against and the
        // regions the grid's pointer field tags an address with.
        void set_modules(std::vector<process::ModuleInfo> modules, std::vector<process::RegionInfo> regions);

        [[nodiscard]] models::ReclassStructureModel* structure() const noexcept;
        [[nodiscard]] models::ReclassProxyModel*     proxy() const noexcept;
        [[nodiscard]] components::ProjectView*       project_view() const noexcept;
        [[nodiscard]] components::ReClassView*       grid() const noexcept;
        [[nodiscard]] QLabel*                        header_label() const noexcept;
        [[nodiscard]] widgets::StatusLabel*          status_label() const noexcept;

        [[nodiscard]] QMenuBar*   menu_bar() const noexcept;
        [[nodiscard]] QToolBar*   tool_bar() const noexcept;
        [[nodiscard]] QStatusBar* status_bar() const noexcept;

        [[nodiscard]] QAction*     new_class_action() const noexcept;
        [[nodiscard]] QAction*     delete_class_action() const noexcept;
        [[nodiscard]] QToolButton* add_node_button() const noexcept;
        [[nodiscard]] QAction*     delete_node_action() const noexcept;
        [[nodiscard]] QAction*     dissect_action() const noexcept;
        [[nodiscard]] QAction*     follow_action() const noexcept;
        [[nodiscard]] QAction*     hierarchy_action() const noexcept;

        [[nodiscard]] std::vector<LiveRequest> next_live_request() override;
        void                                   apply_live_readings(std::span<const LiveReading> readings) override;

        // The window's commands, shared by the menu strip, the toolbar, the node
        // menus and the tests.
        void add_class();
        void rename_class();
        void set_class_address();
        void delete_class();
        void remove_unused_classes();

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

        // Appends behind the selected node's container, or inserts in front of
        // the selected node; the prompt-free overloads are the presets.
        void               add_bytes();
        void               insert_bytes();
        [[nodiscard]] bool add_bytes(std::size_t count);
        [[nodiscard]] bool insert_bytes(std::size_t count);

        // Turns the selected nodes into a class of their own and leaves a class
        // instance in their place, the reference's "Create Class from Nodes".
        void create_class_from_nodes();

        void copy_address();
        void shrink_class();

        void edit_name();
        void edit_comment();
        void set_node_hex(bool hex);
        void copy_name();
        void remove_selected_nodes();

        // Fills the grid's row menu, so a test can inspect it without opening a
        // modal box.
        void populate_row_menu(QMenu& menu);

        // Hide and unhide, the reference's five commands. A hidden node collapses
        // to a single `...` row the grid unhides on click.
        void hide_selected_nodes();
        void unhide_selected_nodes();
        void unhide_child_nodes();
        void unhide_above_nodes();
        void unhide_below_nodes();

        void                      copy_selected_nodes();
        void                      paste_nodes();
        [[nodiscard]] std::size_t clipboard_size() const noexcept;

        void               set_class_hierarchy_view(bool enabled);
        [[nodiscard]] bool class_hierarchy_view() const noexcept;

        // The enum commands; the prompt-free ones let a test drive them without
        // opening a modal box.
        void add_enum();
        void edit_enum(std::size_t index);
        void delete_enum(std::size_t index);

        [[nodiscard]] bool                 create_enum(const QString& name, std::size_t size);
        [[nodiscard]] bool                 remove_enum(const QString& name);
        [[nodiscard]] std::vector<QString> enum_names() const;

        [[nodiscard]] std::optional<std::size_t> current_node_id() const;
        [[nodiscard]] std::vector<std::size_t>   selected_node_ids() const;

        // Re-reads the project after the table that carries it was replaced.
        void refresh_project();

    signals:
        // The value of the selected node, for the scanner's equal-value search.
        void searchRequested(const QString& text, int bytes);

        // The selected node's address, for the Access Watch.
        void accessWatchRequested(std::uint64_t address, std::size_t width, slopkit::debug::Kind kind);

    protected:
        void closeEvent(QCloseEvent* event) override;

    private:
        void build_layout();
        void connect_signals();

        void     build_menu_bar();
        void     build_tool_bar();
        void     build_status_bar();
        QWidget* build_grid_pane();

        void                 populate_node_kind_menu(QMenu& menu, bool change);
        void                 populate_add_bytes_menu(QMenu& menu, bool insert);
        [[nodiscard]] QMenu* build_add_node_menu();
        [[nodiscard]] QMenu* build_add_bytes_menu(bool insert);

        void show_row_menu(const QPoint& position);

        void on_class_selected(std::size_t index);

        // The project or the selected class changed: the tree, the grid and the
        // remembered cursor all follow.
        void refresh_all();
        void refresh_project_tree();
        void refresh_header();
        void refresh_class_tooltips();
        void update_actions();

        void report(const QString& message, bool error = false);

        [[nodiscard]] bool    selected_class_exists() const;
        [[nodiscard]] QString validate_expression(const QString& text) const;
        [[nodiscard]] QString class_name_error(const QString& name, const reclass::Class* current) const;

        [[nodiscard]] std::vector<const models::ReclassRow*> selected_rows() const;
        [[nodiscard]] reclass::Node*                         node_at(const models::ReclassRow* row) const;

        // The container the byte commands work on and the index in it, taken from
        // the selection: the selected container itself, or the container that
        // holds the selected node.
        struct ContainerTarget
        {
            std::vector<reclass::NodePtr>* nodes {};
            std::size_t                    index {};
        };

        [[nodiscard]] std::optional<ContainerTarget> byte_target(bool insert);

        // Appends `count` padding nodes at the target's index, each with a fresh
        // id.
        void insert_padding(const ContainerTarget& target, std::size_t count);

        // The prompt the "… Bytes" menu entries open.
        [[nodiscard]] std::optional<std::size_t> prompt_byte_count(const QString& title);

        // Unhides the consecutive hidden nodes above or below the selected one,
        // the reference's UnhideNodesAbove/UnhideNodesBelow.
        void unhide_run(bool above);

        // Hands the selected node's value to the scanner and its address to the
        // Access Watch.
        void search_equal_values();
        void watch_address(bool writes);

        // One hex node the dissect command is guessing a kind for; the node is
        // named by its id, so a container's element can be dissected as well as
        // a class's own field.
        struct DissectTarget
        {
            std::size_t            node_id {};
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

        components::ProjectView* project_view_ {};
        components::ReClassView* grid_ {};
        QLabel*                  header_ {};
        widgets::StatusLabel*    status_ {};
        QLabel*                  process_label_ {};
        QPushButton*             close_ {};

        QMenuBar*   menu_bar_ {};
        QToolBar*   tool_bar_ {};
        QStatusBar* status_bar_ {};

        QAction*     new_class_ {};
        QAction*     delete_class_ {};
        QAction*     delete_node_ {};
        QAction*     delete_action_ {};
        QAction*     dissect_ {};
        QAction*     follow_ {};
        QAction*     hierarchy_ {};
        QToolButton* add_node_ {};

        // The nodes Copy put on the clipboard, cloned so editing the project does
        // not touch them.
        std::vector<reclass::NodePtr> clipboard_;

        std::optional<process::JobId> dissect_job_;
        std::vector<DissectTarget>    dissect_targets_;
    };

}
