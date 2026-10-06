#include "ui/panels/debugger_panel.hpp"

#include <cstdint>
#include <vector>

#include <QHeaderView>
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
        void apply_fixed_font(QTableView* table)
        {
            table->setFont(mono_font());
        }
    } // namespace

    DebuggerPanel::DebuggerPanel(debug::Controller& controller, QWidget* parent)
        : QWidget(parent), controller_(controller)
    {
        build_layout();

        connect(&controller_, &debug::Controller::stateChanged, this, &DebuggerPanel::sync_editable);
        connect(&controller_,
                &debug::Controller::registersChanged,
                this,
                [this]
                {
                    refresh_registers();
                    sync_editable();
                });
        connect(&controller_, &debug::Controller::backtraceChanged, this, &DebuggerPanel::refresh_backtrace);

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

        sync_editable();
        refresh_registers();
        refresh_backtrace();
    }

    void DebuggerPanel::build_layout()
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);

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

        // The pane's tables keep the keyboard reachable in order; the dialog
        // links the control bar's last button to the register table.
        widgets::chain_tab_order({register_table_, call_stack_table_});
    }

    void DebuggerPanel::sync_editable()
    {
        register_model_->set_editable(controller_.state() == debug::Controller::State::stopped);
    }

    void DebuggerPanel::refresh()
    {
        sync_editable();
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

    QTableView* DebuggerPanel::register_table() const noexcept
    {
        return register_table_;
    }

    QTableView* DebuggerPanel::call_stack_table() const noexcept
    {
        return call_stack_table_;
    }

} // namespace slopkit::ui::panels
