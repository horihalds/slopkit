#include "ui/dialogs/referenced_strings.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableView>
#include <QVBoxLayout>

#include "ui/components/elided_tooltip_delegate.hpp"
#include "ui/components/widgets.hpp"

namespace slopkit::ui::dialogs
{
    ReferencedStringsDialog::ReferencedStringsDialog(const components::ReferencedStringTable& table)
        : QDialog(nullptr), table_(table)
    {
        setWindowTitle(tr("Referenced strings"));
        setWindowFlag(Qt::Window, true);
        resize(760, 460);
        // A companion window: it must not own the process lifetime, or closing
        // the shell would leave slopkit running.
        setAttribute(Qt::WA_QuitOnClose, false);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(6);

        layout->addWidget(widgets::section_header(tr("Referenced strings"), this));

        auto* controls = new QHBoxLayout();
        controls->setSpacing(6);
        controls->addWidget(new QLabel(tr("Module:"), this));
        module_combo_ = new QComboBox(this);
        module_combo_->setObjectName(QStringLiteral("referenced_strings_module"));
        controls->addWidget(module_combo_);
        filter_ = new QLineEdit(this);
        filter_->setObjectName(QStringLiteral("referenced_strings_filter"));
        filter_->setPlaceholderText(tr("Filter strings"));
        filter_->setClearButtonEnabled(true);
        controls->addWidget(filter_, 1);
        analyze_ = widgets::secondary_button(tr("Analyze Module..."), this);
        analyze_->setObjectName(QStringLiteral("referenced_strings_analyze"));
        controls->addWidget(analyze_);
        layout->addLayout(controls);

        model_      = new models::ReferencedStringModel(this,
                                                        [](std::uint64_t address)
                                                        {
                                                       return ui::format_absolute(address);
                                                        });
        table_view_ = new QTableView(this);
        table_view_->setObjectName(QStringLiteral("referenced_strings_table"));
        table_view_->setModel(model_);
        table_view_->setItemDelegate(new widgets::ElidedTooltipDelegate(table_view_));
        table_view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_view_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_view_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_view_->setShowGrid(false);
        table_view_->verticalHeader()->setVisible(false);
        table_view_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table_view_->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(table_view_, 1);

        empty_state_ = new QLabel(this);
        empty_state_->setObjectName(QStringLiteral("referenced_strings_empty"));
        empty_state_->setWordWrap(true);
        empty_state_->setAlignment(Qt::AlignCenter);
        empty_state_->setText(tr("No referenced strings yet. Use Analyze Module... to scan a module."));
        layout->addWidget(empty_state_, 1);

        status_ = new widgets::StatusLabel(this);
        status_->setObjectName(QStringLiteral("referenced_strings_status"));
        layout->addWidget(status_);

        connect(analyze_, &QPushButton::clicked, this, &ReferencedStringsDialog::analyzeRequested);
        connect(module_combo_, &QComboBox::currentIndexChanged, this, &ReferencedStringsDialog::rebuild_rows);
        connect(filter_, &QLineEdit::textChanged, this, &ReferencedStringsDialog::rebuild_rows);
        connect(table_view_,
                &QTableView::doubleClicked,
                this,
                [this](const QModelIndex& index)
                {
                    if (index.isValid())
                    {
                        emit instructionActivated(model_->instruction_at(index.row()));
                    }
                });

        refresh();
    }

    void ReferencedStringsDialog::refresh()
    {
        const QString previous = module_combo_->currentText();

        {
            const QSignalBlocker blocker(module_combo_);
            module_combo_->clear();
            module_combo_->addItem(tr("All analyzed modules"));
            for (const std::string& name : table_.modules())
            {
                module_combo_->addItem(QString::fromStdString(name));
            }
            const int index = module_combo_->findText(previous);
            module_combo_->setCurrentIndex(index > 0 ? index : 0);
        }

        const bool has_entries = !table_.empty();
        table_view_->setVisible(has_entries);
        empty_state_->setVisible(!has_entries);

        rebuild_rows();
    }

    void ReferencedStringsDialog::rebuild_rows()
    {
        const QString module = selected_module();
        const QString filter = filter_->text();

        std::vector<models::ReferencedStringRow> rows;
        std::size_t                              total = 0;
        for (const std::string& name : table_.modules())
        {
            const QString label = QString::fromStdString(name);
            if (!module.isEmpty() && label != module)
            {
                continue;
            }
            for (const components::ReferencedString& entry : table_.module_entries(name))
            {
                ++total;
                const QString text = QString::fromStdString(entry.text);
                if (!filter.isEmpty() && !text.contains(filter, Qt::CaseInsensitive))
                {
                    continue;
                }
                rows.push_back(models::ReferencedStringRow {.text           = text,
                                                            .string_address = entry.string_address,
                                                            .module         = label,
                                                            .instruction    = entry.instruction});
            }
        }

        model_->set_rows(std::move(rows));
        status_->set_status(widgets::StatusKind::info,
                            tr("Showing %1 of %2 string(s).").arg(model_->row_count()).arg(static_cast<int>(total)));
    }

    QString ReferencedStringsDialog::selected_module() const
    {
        return module_combo_->currentIndex() <= 0 ? QString() : module_combo_->currentText();
    }

    QComboBox* ReferencedStringsDialog::module_combo() const noexcept
    {
        return module_combo_;
    }

    QLineEdit* ReferencedStringsDialog::filter_edit() const noexcept
    {
        return filter_;
    }

    QTableView* ReferencedStringsDialog::table() const noexcept
    {
        return table_view_;
    }

    QPushButton* ReferencedStringsDialog::analyze_button() const noexcept
    {
        return analyze_;
    }

    QLabel* ReferencedStringsDialog::empty_state() const noexcept
    {
        return empty_state_;
    }

    QString ReferencedStringsDialog::status_text() const
    {
        return status_->text();
    }

    int ReferencedStringsDialog::row_count() const
    {
        return model_->row_count();
    }

    std::uint64_t ReferencedStringsDialog::instruction_at(int row) const
    {
        return model_->instruction_at(row);
    }

    QString ReferencedStringsDialog::cell_text(int row, int column) const
    {
        return model_->index(row, column).data(Qt::DisplayRole).toString();
    }

    void ReferencedStringsDialog::select_module(const QString& name)
    {
        const int index = module_combo_->findText(name);
        module_combo_->setCurrentIndex(index > 0 ? index : 0);
    }

    void ReferencedStringsDialog::set_filter(const QString& text)
    {
        filter_->setText(text);
        rebuild_rows();
    }

}
