#pragma once

#include <cstdint>
#include <optional>

#include <QMenuBar>
#include <QString>

#include "debug/controller.hpp"
#include "process/attachment.hpp"

class QAction;
class QMenu;

namespace slopkit::ui::components
{
    class MemoryView;
} // namespace slopkit::ui::components

namespace slopkit::ui::panels
{
    // The Memory Viewer's own menu bar: `File`, `View` and `Tools` carry the
    // viewer's commands, `Debug` mirrors the one-line debug control bar it sits
    // above. Like the bar it owns the debugger gating (through
    // `debug_enablement()`) and submits every debug command to the controller
    // itself. The viewer's own commands are rendered from `CommandState` and
    // emitted for the dialog, which owns the selection, the focus and the panes.
    class ViewerMenu : public QMenuBar
    {
        Q_OBJECT

    public:
        // What the viewer's own entries may act on right now. The dialog derives
        // it from the listing's selection and the focused pane, so the menu and
        // the panes can never disagree about a command.
        struct CommandState
        {
            bool    can_go_back {};                 // the focused pane's history is not empty
            bool    can_follow {};                  // the selected row references an address
            bool    selected_is_instruction {};     // decoded and not covered by a session patch
            bool    selected_is_patched {};         // inside a session patch
            bool    selected_has_memory_operand {}; // the selected row accesses memory
            QString restore_description;            // the replaced instruction, for its tooltip
        };

        ViewerMenu(debug::Controller&             controller,
                   const process::AttachedTarget& target,
                   components::MemoryView&        view, // its "Go To..." action leads the View menu
                   QWidget*                       parent = nullptr);

        // The instruction `Toggle Breakpoint` acts on, exactly as on the bar.
        void set_selected_instruction(std::optional<std::uint64_t> address, const QString& expression);
        // Pushes the viewer's own gating; the dialog refreshes it on a selection
        // change and before the View/Tools menus open.
        void set_command_state(const CommandState& state);

        // Re-applies the Debug menu's gating after the attached target changed;
        // the Start/Stop entry depends on target validity, which no controller
        // signal announces.
        void refresh_target_state();

        [[nodiscard]] QMenu* file_menu() const noexcept;
        [[nodiscard]] QMenu* view_menu() const noexcept;
        [[nodiscard]] QMenu* tools_menu() const noexcept;
        [[nodiscard]] QMenu* debug_menu() const noexcept;

        [[nodiscard]] QAction* start_stop_action() const noexcept;
        [[nodiscard]] QAction* toggle_breakpoint_action() const noexcept;
        [[nodiscard]] QAction* resume_action() const noexcept;
        [[nodiscard]] QAction* break_action() const noexcept;
        [[nodiscard]] QAction* step_into_action() const noexcept;
        [[nodiscard]] QAction* step_over_action() const noexcept;
        [[nodiscard]] QAction* step_out_action() const noexcept;
        [[nodiscard]] QAction* breakpoints_action() const noexcept;
        [[nodiscard]] QAction* remove_all_breakpoints_action() const noexcept;
        [[nodiscard]] QAction* close_action() const noexcept;
        [[nodiscard]] QAction* follow_action() const noexcept;
        [[nodiscard]] QAction* follow_in_memory_view_action() const noexcept;
        [[nodiscard]] QAction* back_action() const noexcept;
        [[nodiscard]] QAction* nop_action() const noexcept;
        [[nodiscard]] QAction* restore_action() const noexcept;
        [[nodiscard]] QAction* edit_action() const noexcept;
        [[nodiscard]] QAction* instruction_accesses_action() const noexcept;

    signals:
        void breakpointsRequested(); // relayed to the bar so MainWindow's wiring covers both
        void toggleBreakpointRequested(std::uint64_t address, const QString& expression);
        void closeRequested();
        void backRequested(); // the dialog picks the focused pane
        void followRequested();
        void followInMemoryViewRequested();
        void nopRequested();
        void restoreRequested();
        void editRequested();
        void instructionAccessesRequested();

    private:
        void build_menus();
        // Recomputes the Debug menu from the session state, exactly as the bar.
        void apply_debug_state();
        // Recomputes the toggle entry from the session state and the selection.
        void update_toggle_state();
        // Maps the last CommandState onto the View/Tools entries.
        void refresh_commands();
        void toggle_session();

        debug::Controller&             controller_;
        const process::AttachedTarget& target_;
        components::MemoryView&        view_;

        QMenu* file_menu_ {};
        QMenu* view_menu_ {};
        QMenu* tools_menu_ {};
        QMenu* debug_menu_ {};

        QAction* start_stop_action_ {};
        QAction* toggle_breakpoint_action_ {};
        QAction* resume_action_ {};
        QAction* break_action_ {};
        QAction* step_into_action_ {};
        QAction* step_over_action_ {};
        QAction* step_out_action_ {};
        QAction* breakpoints_action_ {};
        QAction* remove_all_breakpoints_action_ {};
        QAction* close_action_ {};
        QAction* follow_action_ {};
        QAction* follow_in_memory_view_action_ {};
        QAction* back_action_ {};
        QAction* nop_action_ {};
        QAction* restore_action_ {};
        QAction* edit_action_ {};
        QAction* instruction_accesses_action_ {};

        CommandState                 command_state_;
        std::optional<std::uint64_t> selected_address_;
        QString                      selected_expression_;
    };

} // namespace slopkit::ui::panels
