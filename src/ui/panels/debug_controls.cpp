#include "ui/panels/debug_controls.hpp"

#include <QHBoxLayout>
#include <QPushButton>

#include "ui/address_format.hpp"
#include "ui/components/widgets.hpp"
#include "ui/panels/debug_enablement.hpp"

namespace slopkit::ui::panels
{

    DebugControls::DebugControls(debug::Controller& controller, const process::AttachedTarget& target, QWidget* parent)
        : QWidget(parent), controller_(controller), target_(target)
    {
        build_layout();

        connect(&controller_, &debug::Controller::stateChanged, this, &DebugControls::apply_state);
        connect(&controller_, &debug::Controller::breakpointsChanged, this, &DebugControls::update_toggle_state);

        apply_state();
    }

    void DebugControls::build_layout()
    {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);

        // The one dynamic session toggle leads the row: `Start Debugging` while
        // idle, `Stop Debugging` while a session is open.
        start_stop_button_        = new widgets::PrimaryButton(tr("Start Debugging"), this);
        toggle_breakpoint_button_ = widgets::secondary_button(tr("Toggle Breakpoint"), this);
        resume_button_            = widgets::secondary_button(tr("Resume"), this);
        break_button_             = widgets::secondary_button(tr("Break"), this);
        step_into_button_         = widgets::secondary_button(tr("Step Into"), this);
        step_over_button_         = widgets::secondary_button(tr("Step Over"), this);
        step_out_button_          = widgets::secondary_button(tr("Step Out"), this);
        breakpoints_button_       = widgets::secondary_button(tr("Breakpoints..."), this);

        layout->addWidget(start_stop_button_);
        layout->addWidget(toggle_breakpoint_button_);
        layout->addWidget(resume_button_);
        layout->addWidget(break_button_);
        layout->addWidget(step_into_button_);
        layout->addWidget(step_over_button_);
        layout->addWidget(step_out_button_);
        layout->addWidget(breakpoints_button_);

        // Step Out has no implementation yet (no controller step_out and no
        // backend run-to-address), so the button documents the gap and never
        // comes live.
        step_out_button_->setEnabled(false);
        step_out_button_->setToolTip(tr("Stepping out is not supported yet."));

        layout->addStretch(1);

        connect(toggle_breakpoint_button_,
                &QPushButton::clicked,
                this,
                [this]
                {
                    if (selected_address_.has_value())
                    {
                        emit toggleBreakpointRequested(*selected_address_, selected_expression_);
                    }
                });
        connect(start_stop_button_, &QPushButton::clicked, this, &DebugControls::toggle_session);
        connect(resume_button_, &QPushButton::clicked, &controller_, &debug::Controller::resume);
        connect(break_button_, &QPushButton::clicked, &controller_, &debug::Controller::interrupt);
        connect(step_into_button_, &QPushButton::clicked, &controller_, &debug::Controller::step_into);
        connect(step_over_button_, &QPushButton::clicked, &controller_, &debug::Controller::step_over);
        connect(breakpoints_button_, &QPushButton::clicked, this, &DebugControls::breakpointsRequested);

        // The controls stay reachable with the keyboard; the dialog appends the
        // register and call-stack tables after these.
        widgets::chain_tab_order({start_stop_button_,
                                  toggle_breakpoint_button_,
                                  resume_button_,
                                  break_button_,
                                  step_into_button_,
                                  step_over_button_,
                                  step_out_button_,
                                  breakpoints_button_});
    }

    void DebugControls::set_selected_instruction(std::optional<std::uint64_t> address, const QString& expression)
    {
        selected_address_    = address;
        selected_expression_ = expression;
        update_toggle_state();
    }

    void DebugControls::toggle_session()
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

    void DebugControls::apply_state()
    {
        const auto state = controller_.state();
        const bool idle  = state == debug::Controller::State::idle;

        const DebugEnablement enablement = debug_enablement(state, target_.valid(), false, false);

        start_stop_button_->setText(idle ? tr("Start Debugging") : tr("Stop Debugging"));
        start_stop_button_->setEnabled(enablement.start_stop);
        resume_button_->setEnabled(enablement.resume);
        break_button_->setEnabled(enablement.interrupt);
        step_into_button_->setEnabled(enablement.step_into);
        step_over_button_->setEnabled(enablement.step_over);

        update_toggle_state();
    }

    void DebugControls::update_toggle_state()
    {
        const bool session       = controller_.state() != debug::Controller::State::idle;
        const bool has_selection = selected_address_.has_value();

        const DebugEnablement enablement = debug_enablement(controller_.state(), target_.valid(), has_selection, false);
        toggle_breakpoint_button_->setEnabled(enablement.toggle_breakpoint);

        if (!session)
        {
            toggle_breakpoint_button_->setToolTip(tr("Start the debugger to set breakpoints."));
        }
        else if (!has_selection)
        {
            toggle_breakpoint_button_->setToolTip(tr("Select an instruction in the listing first."));
        }
        else
        {
            const QString address  = ui::format_padded_hex(*selected_address_);
            const bool    set_here = controller_.table().software_at(*selected_address_) != nullptr;
            toggle_breakpoint_button_->setToolTip(set_here ? tr("Remove the breakpoint at %1.").arg(address)
                                                           : tr("Set a breakpoint at %1.").arg(address));
        }
    }

    QPushButton* DebugControls::toggle_breakpoint_button() const noexcept
    {
        return toggle_breakpoint_button_;
    }

    QPushButton* DebugControls::start_stop_button() const noexcept
    {
        return start_stop_button_;
    }

    QPushButton* DebugControls::resume_button() const noexcept
    {
        return resume_button_;
    }

    QPushButton* DebugControls::break_button() const noexcept
    {
        return break_button_;
    }

    QPushButton* DebugControls::step_into_button() const noexcept
    {
        return step_into_button_;
    }

    QPushButton* DebugControls::step_over_button() const noexcept
    {
        return step_over_button_;
    }

    QPushButton* DebugControls::step_out_button() const noexcept
    {
        return step_out_button_;
    }

    QPushButton* DebugControls::breakpoints_button() const noexcept
    {
        return breakpoints_button_;
    }

} // namespace slopkit::ui::panels
