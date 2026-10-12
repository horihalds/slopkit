#include "ui/dialogs/module_chooser.hpp"

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "ui/components/widgets.hpp"

namespace slopkit::ui::dialogs
{
    QString module_choice_text(const process::ModuleInfo& module)
    {
        return QStringLiteral("%1 (0x%2)").arg(QString::fromStdString(module.name)).arg(module.base, 0, 16);
    }

    ModuleChooserDialog::ModuleChooserDialog(std::vector<process::ModuleInfo> modules, QWidget* parent)
        : QDialog(parent), modules_(std::move(modules))
    {
        setWindowModality(Qt::WindowModal);
        setWindowTitle(tr("Analyze Referenced Strings"));

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(10);

        layout->addWidget(new QLabel(tr("Module to analyze:"), this));

        combo_ = new QComboBox(this);
        combo_->setObjectName(QStringLiteral("module_chooser"));
        for (const process::ModuleInfo& module : modules_)
        {
            combo_->addItem(module_choice_text(module));
        }
        layout->addWidget(combo_);

        layout->addWidget(widgets::hint_text(
            tr("Every instruction whose direct memory operand points at a string in this module gets a comment."),
            this));

        auto* buttons = new QHBoxLayout();
        buttons->setSpacing(6);
        buttons->addStretch(1);

        auto* cancel = widgets::secondary_button(tr("Cancel"), this);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        buttons->addWidget(cancel);

        auto* analyze = new widgets::PrimaryButton(tr("Analyze"), this);
        connect(analyze, &QPushButton::clicked, this, &QDialog::accept);
        buttons->addWidget(analyze);

        layout->addLayout(buttons);
    }

    int ModuleChooserDialog::count() const noexcept
    {
        return static_cast<int>(modules_.size());
    }

    void ModuleChooserDialog::set_current_index(int index)
    {
        combo_->setCurrentIndex(index);
    }

    int ModuleChooserDialog::current_index() const noexcept
    {
        return combo_->currentIndex();
    }

    std::optional<process::ModuleInfo> ModuleChooserDialog::selected_module() const
    {
        const int index = current_index();
        if (index < 0 || static_cast<std::size_t>(index) >= modules_.size())
        {
            return std::nullopt;
        }
        return modules_[static_cast<std::size_t>(index)];
    }

}
