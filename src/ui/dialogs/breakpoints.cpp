#include "ui/dialogs/breakpoints.hpp"

#include <vector>

#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

#include "ui/components/input_box.hpp"
#include "ui/components/widgets.hpp"
#include "ui/models/breakpoint_model.hpp"

namespace slopkit::ui::dialogs
{

    namespace
    {
        constexpr int kKindRole = Qt::UserRole;
    } // namespace

    BreakpointsDialog::BreakpointsDialog(debug::Controller& controller, QWidget* parent)
        : QDialog(parent), controller_(controller)
    {
        setWindowTitle(tr("Breakpoints"));
        setWindowFlag(Qt::Window, true);
        resize(560, 320);
        build_layout();

        connect(&controller_, &debug::Controller::breakpointsChanged, this, &BreakpointsDialog::refresh);
        refresh();
    }

    void BreakpointsDialog::build_layout()
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(6);

        auto* controls = new QHBoxLayout();
        controls->setSpacing(4);

        kind_ = new widgets::ScrollingComboBox(10, this);
        kind_->setObjectName(QStringLiteral("breakpoint_kind"));
        kind_->addItem(tr("Software"), static_cast<int>(debug::Kind::software));
        kind_->addItem(tr("Hardware execute"), static_cast<int>(debug::Kind::hardware_execute));
        kind_->addItem(tr("Hardware write"), static_cast<int>(debug::Kind::hardware_write));
        kind_->addItem(tr("Hardware read/write"), static_cast<int>(debug::Kind::hardware_read_write));

        size_ = new widgets::ScrollingComboBox(10, this);
        size_->setObjectName(QStringLiteral("breakpoint_size"));
        for (const std::size_t size : {1U, 2U, 4U, 8U})
        {
            size_->addItem(tr("%1 byte(s)").arg(size), static_cast<int>(size));
        }

        add_    = widgets::secondary_button(tr("Add Breakpoint..."), this);
        remove_ = widgets::secondary_button(tr("Remove"), this);
        clear_  = widgets::secondary_button(tr("Clear All"), this);

        controls->addWidget(kind_);
        controls->addWidget(size_);
        controls->addWidget(add_);
        controls->addWidget(remove_);
        controls->addWidget(clear_);
        controls->addStretch(1);
        layout->addLayout(controls);

        model_ = new models::BreakpointModel(controller_, this);
        table_ = new QTableView(this);
        table_->setObjectName(QStringLiteral("breakpoint_table"));
        table_->setModel(model_);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table_->setShowGrid(false);
        table_->verticalHeader()->setVisible(false);
        table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table_->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(table_, 1);

        hint_ = new widgets::StatusLabel(this);
        layout->addWidget(hint_);

        connect(kind_,
                &QComboBox::currentIndexChanged,
                this,
                [this]
                {
                    update_hint();
                });
        connect(add_, &QPushButton::clicked, this, &BreakpointsDialog::prompt_add);
        connect(remove_, &QPushButton::clicked, this, &BreakpointsDialog::remove_selected);
        connect(clear_, &QPushButton::clicked, &controller_, &debug::Controller::clear_breakpoints);

        widgets::chain_tab_order({kind_, size_, add_, remove_, clear_, table_});
        update_hint();
    }

    void BreakpointsDialog::prompt_add()
    {
        widgets::InputBoxOptions options;
        options.title       = tr("Add Breakpoint");
        options.label       = tr("Address");
        options.placeholder = tr("401000 or module+1A2B");
        options.monospace   = true;

        const std::optional<QString> text = widgets::get_text(options, this);
        if (!text.has_value())
        {
            return;
        }
        static_cast<void>(add_breakpoint_from_text(*text));
    }

    QString BreakpointsDialog::add_breakpoint_from_text(const QString& expression)
    {
        const auto added = controller_.add_breakpoint(expression.toStdString(), selected_kind(), selected_size());
        if (!added.has_value())
        {
            const QString reason = QString::fromStdString(added.error());
            hint_->set_status(widgets::StatusKind::warning, reason);
            return reason;
        }
        refresh();
        return {};
    }

    void BreakpointsDialog::remove_selected()
    {
        std::vector<std::uint64_t> ids;
        for (const QModelIndex& index : table_->selectionModel()->selectedRows())
        {
            ids.push_back(static_cast<std::uint64_t>(model_->id_at(index.row())));
        }
        for (const std::uint64_t id : ids)
        {
            controller_.remove_breakpoint(id);
        }
    }

    void BreakpointsDialog::refresh()
    {
        model_->refresh();
        update_hint();
    }

    void BreakpointsDialog::update_hint()
    {
        const auto kind = selected_kind();
        const bool data = kind == debug::Kind::hardware_write || kind == debug::Kind::hardware_read_write;
        size_->setEnabled(data);

        if (controller_.state() == debug::Controller::State::idle)
        {
            hint_->set_status(widgets::StatusKind::info, tr("Start a debug session to arm breakpoints."));
        }
        else if (model_->rowCount() == 0)
        {
            hint_->set_status(widgets::StatusKind::info, tr("No breakpoints."));
        }
        else
        {
            hint_->set_status(widgets::StatusKind::info, QString());
        }
    }

    debug::Kind BreakpointsDialog::selected_kind() const
    {
        return static_cast<debug::Kind>(kind_->currentData(kKindRole).toInt());
    }

    std::size_t BreakpointsDialog::selected_size() const
    {
        const int size = size_->currentData(kKindRole).toInt();
        return size > 0 ? static_cast<std::size_t>(size) : 1;
    }

    models::BreakpointModel* BreakpointsDialog::model() const noexcept
    {
        return model_;
    }

    QTableView* BreakpointsDialog::table() const noexcept
    {
        return table_;
    }

    widgets::ScrollingComboBox* BreakpointsDialog::kind_combo() const noexcept
    {
        return kind_;
    }

    widgets::ScrollingComboBox* BreakpointsDialog::size_combo() const noexcept
    {
        return size_;
    }

    QPushButton* BreakpointsDialog::add_button() const noexcept
    {
        return add_;
    }

    QPushButton* BreakpointsDialog::remove_button() const noexcept
    {
        return remove_;
    }

    QPushButton* BreakpointsDialog::clear_button() const noexcept
    {
        return clear_;
    }

    widgets::StatusLabel* BreakpointsDialog::hint_label() const noexcept
    {
        return hint_;
    }

} // namespace slopkit::ui::dialogs
