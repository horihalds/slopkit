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
}

namespace slopkit::ui::panels
{
    class ViewerMenu : public QMenuBar
    {
        Q_OBJECT

    public:
        struct CommandState
        {
            bool    can_go_back {};
            bool    can_follow {};
            bool    selected_is_instruction {};
            bool    selected_is_patched {};
            bool    selected_has_memory_operand {};
            bool    can_hook {};
            QString restore_description;
        };

        ViewerMenu(debug::Controller&             controller,
                   const process::AttachedTarget& target,
                   components::MemoryView&        view,
                   QWidget*                       parent = nullptr);

        void set_selected_instruction(std::optional<std::uint64_t> address, const QString& expression);
        void set_command_state(const CommandState& state);

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
        [[nodiscard]] QAction* hook_action() const noexcept;
        [[nodiscard]] QAction* instruction_accesses_action() const noexcept;

    signals:
        void breakpointsRequested();
        void toggleBreakpointRequested(std::uint64_t address, const QString& expression);
        void closeRequested();
        void backRequested();
        void followRequested();
        void followInMemoryViewRequested();
        void nopRequested();
        void restoreRequested();
        void editRequested();
        void hookRequested();
        void instructionAccessesRequested();

    private:
        void build_menus();
        void apply_debug_state();
        void update_toggle_state();
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
        QAction* hook_action_ {};
        QAction* instruction_accesses_action_ {};

        CommandState                 command_state_;
        std::optional<std::uint64_t> selected_address_;
        QString                      selected_expression_;
    };

}
