#include "ui/panels/debug_controls.hpp"

#include <QHBoxLayout>
#include <QPaintEvent>
#include <QPainter>
#include <QPushButton>
#include <QSizePolicy>

#include "ui/address_format.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"

namespace slopkit::ui::panels
{

    namespace
    {
        widgets::StatusKind status_kind(debug::Controller::MessageKind kind)
        {
            switch (kind)
            {
            case debug::Controller::MessageKind::success:
                return widgets::StatusKind::success;
            case debug::Controller::MessageKind::warning:
                return widgets::StatusKind::warning;
            case debug::Controller::MessageKind::error:
                return widgets::StatusKind::error;
            case debug::Controller::MessageKind::info:
            default:
                return widgets::StatusKind::info;
            }
        }

        // The state read-out of the bar. The one-line bar cannot afford the
        // wrapped status label: it stays on one line and elides its text so the
        // buttons always keep their size hints.
        class ElidedStatusLabel : public widgets::StatusLabel
        {
        public:
            explicit ElidedStatusLabel(QWidget* parent = nullptr) : widgets::StatusLabel(parent)
            {
                setWordWrap(false);
                setMinimumWidth(0);
                setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            }

        protected:
            void paintEvent(QPaintEvent*) override
            {
                QPainter painter(this);
                painter.setPen(palette().color(QPalette::WindowText));
                painter.drawText(rect(),
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 fontMetrics().elidedText(text(), Qt::ElideRight, width()));
            }
        };
    } // namespace

    DebugControls::DebugControls(debug::Controller& controller, const process::AttachedTarget& target, QWidget* parent)
        : QWidget(parent), controller_(controller), target_(target)
    {
        build_layout();

        connect(&controller_, &debug::Controller::stateChanged, this, &DebugControls::apply_state);
        connect(&controller_, &debug::Controller::breakpointsChanged, this, &DebugControls::update_toggle_state);
        connect(&controller_, &debug::Controller::message, this, &DebugControls::apply_message);

        apply_state();
    }

    void DebugControls::build_layout()
    {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);

        toggle_breakpoint_button_ = widgets::secondary_button(tr("Toggle Breakpoint"), this);
        resume_button_            = widgets::secondary_button(tr("Resume"), this);
        break_button_             = widgets::secondary_button(tr("Break"), this);
        step_into_button_         = widgets::secondary_button(tr("Step Into"), this);
        step_over_button_         = widgets::secondary_button(tr("Step Over"), this);
        step_out_button_          = widgets::secondary_button(tr("Step Out"), this);
        start_button_             = new widgets::PrimaryButton(tr("Start Debugging"), this);
        stop_button_              = widgets::secondary_button(tr("Stop Debugging"), this);
        breakpoints_button_       = widgets::secondary_button(tr("Breakpoints..."), this);

        layout->addWidget(toggle_breakpoint_button_);
        layout->addWidget(resume_button_);
        layout->addWidget(break_button_);
        layout->addWidget(step_into_button_);
        layout->addWidget(step_over_button_);
        layout->addWidget(step_out_button_);
        layout->addWidget(start_button_);
        layout->addWidget(stop_button_);
        layout->addWidget(breakpoints_button_);

        // Step Out has no implementation yet (no controller step_out and no
        // backend run-to-address), so the button documents the gap and never
        // comes live.
        step_out_button_->setEnabled(false);
        step_out_button_->setToolTip(tr("Stepping out is not supported yet."));

        layout->addStretch(1);

        status_ = new ElidedStatusLabel(this);
        // The state line reads "Stopped at <address> (breakpoint N)", so it is a
        // mono read-out like the tables of the pane below.
        status_->setFont(mono_font());
        layout->addWidget(status_);

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
        connect(start_button_, &QPushButton::clicked, this, &DebugControls::start_session);
        connect(stop_button_, &QPushButton::clicked, &controller_, &debug::Controller::stop);
        connect(resume_button_, &QPushButton::clicked, &controller_, &debug::Controller::resume);
        connect(break_button_, &QPushButton::clicked, &controller_, &debug::Controller::interrupt);
        connect(step_into_button_, &QPushButton::clicked, &controller_, &debug::Controller::step_into);
        connect(step_over_button_, &QPushButton::clicked, &controller_, &debug::Controller::step_over);
        connect(breakpoints_button_, &QPushButton::clicked, this, &DebugControls::breakpointsRequested);

        // The controls stay reachable with the keyboard; the dialog appends the
        // register and call-stack tables after these.
        widgets::chain_tab_order({toggle_breakpoint_button_,
                                  resume_button_,
                                  break_button_,
                                  step_into_button_,
                                  step_over_button_,
                                  step_out_button_,
                                  start_button_,
                                  stop_button_,
                                  breakpoints_button_});
    }

    void DebugControls::set_selected_instruction(std::optional<std::uint64_t> address, const QString& expression)
    {
        selected_address_    = address;
        selected_expression_ = expression;
        update_toggle_state();
    }

    void DebugControls::report_error(const QString& text)
    {
        status_->set_status(widgets::StatusKind::error, text);
    }

    void DebugControls::start_session()
    {
        if (!target_.valid())
        {
            status_->set_status(widgets::StatusKind::warning, tr("Attach to a target before starting the debugger."));
            return;
        }
        controller_.start(target_.pid, target_.plugin_id);
    }

    void DebugControls::apply_state()
    {
        const auto state    = controller_.state();
        const bool idle     = state == debug::Controller::State::idle;
        const bool stopped  = state == debug::Controller::State::stopped;
        const bool running  = state == debug::Controller::State::running;
        const bool starting = state == debug::Controller::State::starting;

        start_button_->setEnabled(idle && target_.valid());
        stop_button_->setEnabled(!idle && !starting);
        resume_button_->setEnabled(stopped);
        break_button_->setEnabled(running);
        step_into_button_->setEnabled(stopped);
        step_over_button_->setEnabled(stopped);

        status_->set_status(widgets::StatusKind::info, controller_.state_text());
        update_toggle_state();
    }

    void DebugControls::apply_message(debug::Controller::MessageKind kind, const QString& text)
    {
        status_->set_status(status_kind(kind), text);
    }

    void DebugControls::update_toggle_state()
    {
        const bool session       = controller_.state() != debug::Controller::State::idle;
        const bool has_selection = selected_address_.has_value();
        toggle_breakpoint_button_->setEnabled(session && has_selection);

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

    QPushButton* DebugControls::start_button() const noexcept
    {
        return start_button_;
    }

    QPushButton* DebugControls::stop_button() const noexcept
    {
        return stop_button_;
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

    widgets::StatusLabel* DebugControls::status_label() const noexcept
    {
        return status_;
    }

} // namespace slopkit::ui::panels
