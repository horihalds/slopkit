#include "ui/panels/viewer_menu.hpp"

#include <QAction>
#include <QKeySequence>
#include <QMenu>

#include "ui/address_format.hpp"
#include "ui/components/memory_view.hpp"
#include "ui/components/row_menu.hpp"
#include "ui/panels/debug_enablement.hpp"

namespace slopkit::ui::panels
{

    ViewerMenu::ViewerMenu(debug::Controller&             controller,
                           const process::AttachedTarget& target,
                           components::MemoryView&        view,
                           QWidget*                       parent)
        : QMenuBar(parent), controller_(controller), target_(target), view_(view)
    {
        build_menus();

        connect(&controller_, &debug::Controller::stateChanged, this, &ViewerMenu::apply_debug_state);
        connect(&controller_, &debug::Controller::breakpointsChanged, this, &ViewerMenu::apply_debug_state);

        apply_debug_state();
        refresh_commands();
    }

    void ViewerMenu::build_menus()
    {
        file_menu_  = addMenu(tr("File"));
        view_menu_  = addMenu(tr("View"));
        tools_menu_ = addMenu(tr("Tools"));
        debug_menu_ = addMenu(tr("Debug"));

        // Every menu explains its disabled entries through their tooltips.
        for (QMenu* menu : {file_menu_, view_menu_, tools_menu_, debug_menu_})
        {
            widgets::show_explanations(*menu);
        }

        // File: the viewer closes like any other window; hiding it lets the
        // window geometry keeper persist its frame, and the panes keep their history.
        close_action_ = file_menu_->addAction(tr("Close"));
        close_action_->setShortcut(QKeySequence::Close);
        close_action_->setShortcutContext(Qt::WindowShortcut);
        connect(close_action_, &QAction::triggered, this, &ViewerMenu::closeRequested);

        // View: the byte view's own Go To... action leads the menu, so its
        // window-scoped Ctrl+G is added once and never duplicated.
        view_menu_->addAction(view_.goto_action());
        follow_action_ = view_menu_->addAction(tr("Follow"));
        connect(follow_action_, &QAction::triggered, this, &ViewerMenu::followRequested);
        follow_in_memory_view_action_ = view_menu_->addAction(tr("Follow in Memory View"));
        connect(follow_in_memory_view_action_, &QAction::triggered, this, &ViewerMenu::followInMemoryViewRequested);
        view_menu_->addSeparator();
        back_action_ = view_menu_->addAction(tr("Back"));
        connect(back_action_, &QAction::triggered, this, &ViewerMenu::backRequested);

        // Tools: the listing's row commands, acting on the selected instruction.
        nop_action_ = tools_menu_->addAction(tr("NOP Instruction"));
        connect(nop_action_, &QAction::triggered, this, &ViewerMenu::nopRequested);
        restore_action_ = tools_menu_->addAction(tr("Restore Original Instruction"));
        connect(restore_action_, &QAction::triggered, this, &ViewerMenu::restoreRequested);
        edit_action_ = tools_menu_->addAction(tr("Edit Instruction..."));
        connect(edit_action_, &QAction::triggered, this, &ViewerMenu::editRequested);
        tools_menu_->addSeparator();
        instruction_accesses_action_ = tools_menu_->addAction(tr("Find out what addresses this instruction accesses"));
        connect(instruction_accesses_action_, &QAction::triggered, this, &ViewerMenu::instructionAccessesRequested);

        // Debug: the control bar's entries, entry for entry, plus the one new
        // command. Each submission goes to the controller, never to the target.
        start_stop_action_ = debug_menu_->addAction(tr("Start Debugging"));
        connect(start_stop_action_, &QAction::triggered, this, &ViewerMenu::toggle_session);

        toggle_breakpoint_action_ = debug_menu_->addAction(tr("Toggle Breakpoint"));
        connect(toggle_breakpoint_action_,
                &QAction::triggered,
                this,
                [this]
                {
                    if (selected_address_.has_value())
                    {
                        emit toggleBreakpointRequested(*selected_address_, selected_expression_);
                    }
                });

        resume_action_ = debug_menu_->addAction(tr("Resume"));
        connect(resume_action_, &QAction::triggered, &controller_, &debug::Controller::resume);

        break_action_ = debug_menu_->addAction(tr("Break"));
        connect(break_action_, &QAction::triggered, &controller_, &debug::Controller::interrupt);

        step_into_action_ = debug_menu_->addAction(tr("Step Into"));
        connect(step_into_action_, &QAction::triggered, &controller_, &debug::Controller::step_into);

        step_over_action_ = debug_menu_->addAction(tr("Step Over"));
        connect(step_over_action_, &QAction::triggered, &controller_, &debug::Controller::step_over);

        step_out_action_ = debug_menu_->addAction(tr("Step Out"));
        connect(step_out_action_, &QAction::triggered, &controller_, &debug::Controller::step_out);

        // Breakpoints... opens the same window the bar's button does: it is
        // emitted here and relayed to the bar's own request.
        breakpoints_action_ = debug_menu_->addAction(tr("Breakpoints..."));
        connect(breakpoints_action_, &QAction::triggered, this, &ViewerMenu::breakpointsRequested);

        debug_menu_->addSeparator();
        remove_all_breakpoints_action_ = debug_menu_->addAction(tr("Remove All Breakpoints"));
        connect(
            remove_all_breakpoints_action_, &QAction::triggered, &controller_, &debug::Controller::clear_breakpoints);
    }

    void ViewerMenu::set_selected_instruction(std::optional<std::uint64_t> address, const QString& expression)
    {
        selected_address_    = address;
        selected_expression_ = expression;
        update_toggle_state();
    }

    void ViewerMenu::set_command_state(const CommandState& state)
    {
        command_state_ = state;
        refresh_commands();
    }

    void ViewerMenu::toggle_session()
    {
        if (controller_.state() == debug::Controller::State::idle)
        {
            controller_.start(target_.pid, target_.plugin_id);
        }
        else
        {
            controller_.stop();
        }
    }

    void ViewerMenu::refresh_target_state()
    {
        apply_debug_state();
    }

    void ViewerMenu::apply_debug_state()
    {
        const auto state    = controller_.state();
        const bool idle     = state == debug::Controller::State::idle;
        const bool running  = state == debug::Controller::State::running;
        const bool starting = state == debug::Controller::State::starting;

        const DebugEnablement enablement =
            debug_enablement(state, target_.valid(), selected_address_.has_value(), !controller_.table().empty());

        start_stop_action_->setText(idle ? tr("Start Debugging") : tr("Stop Debugging"));
        start_stop_action_->setEnabled(enablement.start_stop);
        start_stop_action_->setToolTip(
            enablement.start_stop ? QString()
                                  : (starting ? tr("The session is starting.") : tr("Attach to a target first.")));

        const QString needs_session = tr("Start the debugger first.");

        resume_action_->setEnabled(enablement.resume);
        resume_action_->setToolTip(enablement.resume ? QString()
                                                     : (running ? tr("Stop the target to step.") : needs_session));

        step_into_action_->setEnabled(enablement.step_into);
        step_into_action_->setToolTip(
            enablement.step_into ? QString() : (running ? tr("Stop the target to step.") : needs_session));

        step_over_action_->setEnabled(enablement.step_over);
        step_over_action_->setToolTip(
            enablement.step_over ? QString() : (running ? tr("Stop the target to step.") : needs_session));

        step_out_action_->setEnabled(enablement.step_out);
        step_out_action_->setToolTip(enablement.step_out ? QString()
                                                         : (running ? tr("Stop the target to step.") : needs_session));

        break_action_->setEnabled(enablement.interrupt);
        break_action_->setToolTip(
            enablement.interrupt
                ? QString()
                : (idle || starting ? needs_session : tr("Only available while the target is running.")));

        remove_all_breakpoints_action_->setEnabled(enablement.clear_breakpoints);
        remove_all_breakpoints_action_->setToolTip(enablement.clear_breakpoints ? QString()
                                                                                : tr("No breakpoints to remove."));

        update_toggle_state();
    }

    void ViewerMenu::update_toggle_state()
    {
        const bool session       = controller_.state() != debug::Controller::State::idle;
        const bool has_selection = selected_address_.has_value();

        toggle_breakpoint_action_->setEnabled(
            debug_enablement(controller_.state(), target_.valid(), has_selection, false).toggle_breakpoint);

        if (!session)
        {
            toggle_breakpoint_action_->setToolTip(tr("Start the debugger to set breakpoints."));
        }
        else if (!has_selection)
        {
            toggle_breakpoint_action_->setToolTip(tr("Select an instruction in the listing first."));
        }
        else
        {
            const QString address  = ui::format_padded_hex(*selected_address_);
            const bool    set_here = controller_.table().software_at(*selected_address_) != nullptr;
            toggle_breakpoint_action_->setToolTip(set_here ? tr("Remove the breakpoint at %1.").arg(address)
                                                           : tr("Set a breakpoint at %1.").arg(address));
        }
    }

    void ViewerMenu::refresh_commands()
    {
        follow_action_->setEnabled(command_state_.can_follow);
        follow_action_->setToolTip(command_state_.can_follow
                                       ? tr("Move the listing's cursor to the address this instruction references.")
                                       : tr("Select an instruction that references an address."));

        follow_in_memory_view_action_->setEnabled(command_state_.can_follow);
        follow_in_memory_view_action_->setToolTip(
            command_state_.can_follow ? tr("Show the address this instruction references in the byte view.")
                                      : tr("Select an instruction that references an address."));

        back_action_->setEnabled(command_state_.can_go_back);
        back_action_->setToolTip(command_state_.can_go_back ? tr("Return the focused pane to its previous location.")
                                                            : tr("The focused pane has nowhere to go back to."));

        nop_action_->setEnabled(command_state_.selected_is_instruction);
        nop_action_->setToolTip(command_state_.selected_is_instruction
                                    ? tr("Replace this instruction with NOP bytes.")
                                    : tr("Only a decoded instruction can be replaced."));

        restore_action_->setEnabled(command_state_.selected_is_patched);
        restore_action_->setToolTip(command_state_.selected_is_patched && !command_state_.restore_description.isEmpty()
                                        ? command_state_.restore_description
                                        : tr("Only a row this session replaced can be restored."));

        edit_action_->setEnabled(command_state_.selected_is_instruction);
        edit_action_->setToolTip(command_state_.selected_is_instruction
                                     ? tr("Rewrite this instruction with assembler text.")
                                     : tr("Only a decoded instruction can be edited."));

        instruction_accesses_action_->setEnabled(command_state_.selected_has_memory_operand);
        instruction_accesses_action_->setToolTip(
            command_state_.selected_has_memory_operand
                ? tr("Resolve this instruction's memory operands from live registers; the debugger is attached first "
                     "(after a confirmation) when no session is running.")
                : tr("Only an instruction with a memory operand can be resolved."));
    }

    QMenu* ViewerMenu::file_menu() const noexcept
    {
        return file_menu_;
    }

    QMenu* ViewerMenu::view_menu() const noexcept
    {
        return view_menu_;
    }

    QMenu* ViewerMenu::tools_menu() const noexcept
    {
        return tools_menu_;
    }

    QMenu* ViewerMenu::debug_menu() const noexcept
    {
        return debug_menu_;
    }

    QAction* ViewerMenu::start_stop_action() const noexcept
    {
        return start_stop_action_;
    }

    QAction* ViewerMenu::toggle_breakpoint_action() const noexcept
    {
        return toggle_breakpoint_action_;
    }

    QAction* ViewerMenu::resume_action() const noexcept
    {
        return resume_action_;
    }

    QAction* ViewerMenu::break_action() const noexcept
    {
        return break_action_;
    }

    QAction* ViewerMenu::step_into_action() const noexcept
    {
        return step_into_action_;
    }

    QAction* ViewerMenu::step_over_action() const noexcept
    {
        return step_over_action_;
    }

    QAction* ViewerMenu::step_out_action() const noexcept
    {
        return step_out_action_;
    }

    QAction* ViewerMenu::breakpoints_action() const noexcept
    {
        return breakpoints_action_;
    }

    QAction* ViewerMenu::remove_all_breakpoints_action() const noexcept
    {
        return remove_all_breakpoints_action_;
    }

    QAction* ViewerMenu::close_action() const noexcept
    {
        return close_action_;
    }

    QAction* ViewerMenu::follow_action() const noexcept
    {
        return follow_action_;
    }

    QAction* ViewerMenu::follow_in_memory_view_action() const noexcept
    {
        return follow_in_memory_view_action_;
    }

    QAction* ViewerMenu::back_action() const noexcept
    {
        return back_action_;
    }

    QAction* ViewerMenu::nop_action() const noexcept
    {
        return nop_action_;
    }

    QAction* ViewerMenu::restore_action() const noexcept
    {
        return restore_action_;
    }

    QAction* ViewerMenu::edit_action() const noexcept
    {
        return edit_action_;
    }

    QAction* ViewerMenu::instruction_accesses_action() const noexcept
    {
        return instruction_accesses_action_;
    }

} // namespace slopkit::ui::panels
