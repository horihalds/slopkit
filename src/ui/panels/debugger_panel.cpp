#include "ui/panels/debugger_panel.hpp"

#include <cstdint>
#include <vector>

#include <QGridLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

#include "ui/address_format.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"
#include "ui/models/call_stack_model.hpp"
#include "ui/models/register_model.hpp"

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

        void apply_fixed_font(QTableView* table)
        {
            table->setFont(mono_font());
        }
    } // namespace

    DebuggerPanel::DebuggerPanel(debug::Controller& controller, const process::AttachedTarget& target, QWidget* parent)
        : QWidget(parent), controller_(controller), target_(target)
    {
        build_layout();

        connect(&controller_, &debug::Controller::stateChanged, this, &DebuggerPanel::apply_state);
        connect(&controller_,
                &debug::Controller::registersChanged,
                this,
                [this]
                {
                    refresh_registers();
                    apply_state();
                });
        connect(&controller_, &debug::Controller::backtraceChanged, this, &DebuggerPanel::refresh_backtrace);
        connect(&controller_, &debug::Controller::message, this, &DebuggerPanel::apply_message);

        register_model_->set_commit_handler(
            [this](const std::string& name, std::uint64_t value)
            {
                if (controller_.state() != debug::Controller::State::stopped)
                {
                    return false;
                }
                controller_.write_register(name, value);
                return true;
            });

        apply_state();
        refresh_registers();
        refresh_backtrace();
    }

    void DebuggerPanel::build_layout()
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);

        auto* controls = new QGridLayout();
        controls->setContentsMargins(0, 0, 0, 0);
        controls->setHorizontalSpacing(4);
        controls->setVerticalSpacing(4);
        start_button_       = new widgets::PrimaryButton(tr("Start Debugging"), this);
        stop_button_        = widgets::secondary_button(tr("Stop Debugging"), this);
        resume_button_      = widgets::secondary_button(tr("Resume"), this);
        break_button_       = widgets::secondary_button(tr("Break"), this);
        step_into_button_   = widgets::secondary_button(tr("Step Into"), this);
        step_over_button_   = widgets::secondary_button(tr("Step Over"), this);
        breakpoints_button_ = widgets::secondary_button(tr("Breakpoints..."), this);
        // Two columns keep the pane able to shrink into its 30% share.
        controls->addWidget(start_button_, 0, 0);
        controls->addWidget(stop_button_, 0, 1);
        controls->addWidget(resume_button_, 1, 0);
        controls->addWidget(break_button_, 1, 1);
        controls->addWidget(step_into_button_, 2, 0);
        controls->addWidget(step_over_button_, 2, 1);
        controls->addWidget(breakpoints_button_, 3, 0);
        controls->setColumnStretch(2, 1);
        layout->addLayout(controls);

        status_ = new widgets::StatusLabel(this);
        // The state line reads "Stopped at <address> (breakpoint N)", so it is a mono
        // read-out like the tables below it.
        status_->setFont(mono_font());
        layout->addWidget(status_);

        layout->addWidget(widgets::section_header(tr("Registers"), this));
        register_model_ = new models::RegisterModel(this);
        register_table_ = new QTableView(this);
        register_table_->setObjectName(QStringLiteral("register_table"));
        register_table_->setModel(register_model_);
        register_table_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed
                                         | QAbstractItemView::AnyKeyPressed);
        register_table_->setSelectionMode(QAbstractItemView::SingleSelection);
        register_table_->setShowGrid(false);
        register_table_->verticalHeader()->setVisible(false);
        register_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        register_table_->horizontalHeader()->setStretchLastSection(true);
        register_table_->horizontalHeader()->resizeSection(0, 80);
        register_table_->horizontalHeader()->resizeSection(1, 170);
        apply_fixed_font(register_table_);
        layout->addWidget(register_table_, 3);

        layout->addWidget(widgets::section_header(tr("Call stack"), this));
        call_stack_model_ = new models::CallStackModel(this);
        call_stack_table_ = new QTableView(this);
        call_stack_table_->setObjectName(QStringLiteral("call_stack_table"));
        call_stack_table_->setModel(call_stack_model_);
        call_stack_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        call_stack_table_->setSelectionMode(QAbstractItemView::NoSelection);
        call_stack_table_->setShowGrid(false);
        call_stack_table_->verticalHeader()->setVisible(false);
        call_stack_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        call_stack_table_->horizontalHeader()->setStretchLastSection(true);
        call_stack_table_->horizontalHeader()->resizeSection(0, 28);
        call_stack_table_->horizontalHeader()->resizeSection(1, 150);
        apply_fixed_font(call_stack_table_);
        layout->addWidget(call_stack_table_, 2);

        connect(start_button_, &QPushButton::clicked, this, &DebuggerPanel::start_session);
        connect(stop_button_, &QPushButton::clicked, &controller_, &debug::Controller::stop);
        connect(resume_button_, &QPushButton::clicked, &controller_, &debug::Controller::resume);
        connect(break_button_, &QPushButton::clicked, &controller_, &debug::Controller::interrupt);
        connect(step_into_button_, &QPushButton::clicked, &controller_, &debug::Controller::step_into);
        connect(step_over_button_, &QPushButton::clicked, &controller_, &debug::Controller::step_over);
        connect(breakpoints_button_, &QPushButton::clicked, this, &DebuggerPanel::breakpointsRequested);

        // The run controls stay reachable with the keyboard.
        widgets::chain_tab_order({start_button_,
                                  stop_button_,
                                  resume_button_,
                                  break_button_,
                                  step_into_button_,
                                  step_over_button_,
                                  breakpoints_button_,
                                  register_table_,
                                  call_stack_table_});
    }

    void DebuggerPanel::start_session()
    {
        if (!target_.valid())
        {
            status_->set_status(widgets::StatusKind::warning, tr("Attach to a target before starting the debugger."));
            return;
        }
        controller_.start(target_.pid, target_.plugin_id);
    }

    void DebuggerPanel::apply_state()
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
        register_model_->set_editable(stopped);

        status_->set_status(widgets::StatusKind::info, controller_.state_text());
    }

    void DebuggerPanel::apply_message(debug::Controller::MessageKind kind, const QString& text)
    {
        status_->set_status(status_kind(kind), text);
    }

    void DebuggerPanel::refresh()
    {
        apply_state();
        refresh_registers();
        refresh_backtrace();
    }

    void DebuggerPanel::refresh_registers()
    {
        std::vector<models::RegisterValue> values;
        values.reserve(controller_.registers().size());
        for (const debug::RegisterValue& value : controller_.registers())
        {
            values.push_back(models::RegisterValue {
                .name  = value.name,
                .value = ui::format_padded_hex(value.value),
                .read  = true,
            });
        }
        register_model_->set_values(values);
    }

    void DebuggerPanel::refresh_backtrace()
    {
        call_stack_model_->set_frames(controller_.backtrace(), controller_.modules(), controller_.address_mode());
    }

    models::RegisterModel* DebuggerPanel::register_model() const noexcept
    {
        return register_model_;
    }

    models::CallStackModel* DebuggerPanel::call_stack_model() const noexcept
    {
        return call_stack_model_;
    }

    QPushButton* DebuggerPanel::start_button() const noexcept
    {
        return start_button_;
    }

    QPushButton* DebuggerPanel::stop_button() const noexcept
    {
        return stop_button_;
    }

    QPushButton* DebuggerPanel::resume_button() const noexcept
    {
        return resume_button_;
    }

    QPushButton* DebuggerPanel::break_button() const noexcept
    {
        return break_button_;
    }

    QPushButton* DebuggerPanel::step_into_button() const noexcept
    {
        return step_into_button_;
    }

    QPushButton* DebuggerPanel::step_over_button() const noexcept
    {
        return step_over_button_;
    }

    QPushButton* DebuggerPanel::breakpoints_button() const noexcept
    {
        return breakpoints_button_;
    }

    widgets::StatusLabel* DebuggerPanel::status_label() const noexcept
    {
        return status_;
    }

    QTableView* DebuggerPanel::register_table() const noexcept
    {
        return register_table_;
    }

    QTableView* DebuggerPanel::call_stack_table() const noexcept
    {
        return call_stack_table_;
    }

} // namespace slopkit::ui::panels
