#include "ui/dialogs/generate_trainer.hpp"

#include <format>
#include <string>
#include <utility>

#include <QCheckBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "table/address_table.hpp"
#include "ui/components/widgets.hpp"

namespace slopkit::ui::dialogs
{
    GenerateTrainerDialog::GenerateTrainerDialog(trainer::Request request, QWidget* parent)
        : QDialog(parent), request_ {std::move(request)}
    {
        setWindowTitle(tr("Generate Trainer"));
        resize(620, 420);

        if (request_.table != nullptr)
        {
            for (const auto& entry : request_.table->entries())
            {
                if (entry.kind == table::EntryKind::script)
                {
                    ++script_entries_;
                }
                else
                {
                    ++value_entries_;
                }
            }
        }

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        layout->addWidget(widgets::section_header(tr("Generate Trainer"), this));
        layout->addWidget(widgets::hint_text(
            tr("A trainer carries the table's value entries inside one executable and needs neither slopkit, Qt nor a "
               "plugin on the machine it runs on."),
            this));

        auto* targets = new QHBoxLayout();
        linux_box_    = new QCheckBox(tr("Linux executable"), this);
        windows_box_  = new QCheckBox(tr("Windows executable (64-bit)"), this);
        linux_box_->setObjectName(QStringLiteral("generate_trainer_linux"));
        windows_box_->setObjectName(QStringLiteral("generate_trainer_windows"));
        linux_box_->setChecked(request_.linux_target);
        windows_box_->setChecked(request_.windows_target);
        targets->addWidget(linux_box_);
        targets->addWidget(windows_box_);
        targets->addStretch(1);
        layout->addLayout(targets);

        auto* output    = new QHBoxLayout();
        directory_edit_ = new QLineEdit(QString::fromStdString(request_.output_directory.string()), this);
        directory_edit_->setObjectName(QStringLiteral("generate_trainer_directory"));
        auto* browse = widgets::secondary_button(tr("Browse…"), this);
        browse->setObjectName(QStringLiteral("generate_trainer_browse"));
        output->addWidget(new QLabel(tr("Output folder"), this));
        output->addWidget(directory_edit_, 1);
        output->addWidget(browse);
        layout->addLayout(output);

        auto* naming = new QHBoxLayout();
        name_edit_   = new QLineEdit(QString::fromStdString(request_.base_name), this);
        name_edit_->setObjectName(QStringLiteral("generate_trainer_name"));
        naming->addWidget(new QLabel(tr("Base name"), this));
        naming->addWidget(name_edit_, 1);
        layout->addLayout(naming);

        summary_ = widgets::hint_text(QString(), this);
        summary_->setObjectName(QStringLiteral("generate_trainer_summary"));
        layout->addWidget(summary_);

        results_ = new QPlainTextEdit(this);
        results_->setObjectName(QStringLiteral("generate_trainer_results"));
        results_->setReadOnly(true);
        layout->addWidget(results_, 1);

        auto* footer = new QHBoxLayout();
        status_      = new widgets::StatusLabel(this);
        status_->setObjectName(QStringLiteral("generate_trainer_status"));
        footer->addWidget(status_, 1);

        auto* close_button    = widgets::secondary_button(tr("Close"), this);
        auto* generate_button = new widgets::PrimaryButton(tr("Generate"), this);
        close_button->setObjectName(QStringLiteral("generate_trainer_close"));
        generate_button->setObjectName(QStringLiteral("generate_trainer_generate"));
        generate_button_ = generate_button;
        footer->addWidget(close_button);
        footer->addWidget(generate_button);
        layout->addLayout(footer);

        connect(browse, &QPushButton::clicked, this, &GenerateTrainerDialog::browse);
        connect(close_button, &QPushButton::clicked, this, &QDialog::reject);
        connect(generate_button, &QPushButton::clicked, this, &GenerateTrainerDialog::generate);
        connect(directory_edit_, &QLineEdit::textChanged, this, &GenerateTrainerDialog::update_state);
        connect(name_edit_, &QLineEdit::textChanged, this, &GenerateTrainerDialog::update_state);
        connect(linux_box_, &QCheckBox::toggled, this, &GenerateTrainerDialog::update_state);
        connect(windows_box_, &QCheckBox::toggled, this, &GenerateTrainerDialog::update_state);

        update_state();
    }

    QString GenerateTrainerDialog::output_directory() const
    {
        return directory_edit_->text();
    }

    void GenerateTrainerDialog::set_output_directory(const QString& directory)
    {
        directory_edit_->setText(directory);
    }

    QString GenerateTrainerDialog::base_name() const
    {
        return name_edit_->text();
    }

    bool GenerateTrainerDialog::linux_target() const
    {
        return linux_box_->isChecked();
    }

    bool GenerateTrainerDialog::windows_target() const
    {
        return windows_box_->isChecked();
    }

    void GenerateTrainerDialog::set_linux_target(bool enabled)
    {
        linux_box_->setChecked(enabled);
    }

    void GenerateTrainerDialog::set_windows_target(bool enabled)
    {
        windows_box_->setChecked(enabled);
    }

    QString GenerateTrainerDialog::summary_text() const
    {
        return tr("%1 value entries, %2 script entries").arg(value_entries_).arg(script_entries_);
    }

    bool GenerateTrainerDialog::can_generate() const
    {
        return (linux_target() || windows_target()) && !output_directory().isEmpty() && !base_name().isEmpty();
    }

    void GenerateTrainerDialog::generate()
    {
        trainer::Request request = request_;
        request.linux_target     = linux_target();
        request.windows_target   = windows_target();
        request.output_directory = output_directory().toStdString();
        request.base_name        = base_name().toStdString();

        const auto generated = trainer::generate(request);
        if (!generated)
        {
            results_->setPlainText(QString::fromStdString(generated.error()));
            status_->set_status(widgets::StatusKind::error, tr("No trainer was written"));
            log::warning(log::category::ui, std::format("generate trainer failed: {}", generated.error()));
            return;
        }

        QStringList lines;
        for (const auto& written : generated->written)
        {
            lines << tr("%1 (%2)").arg(QString::fromStdString(written.path.string()),
                                       QString::fromStdString(std::string {trainer::target_name(written.target)}));
        }
        for (const auto& message : generated->errors)
        {
            lines << QString::fromStdString(message);
        }
        results_->setPlainText(lines.join(QLatin1Char('\n')));

        for (const auto& message : generated->errors)
        {
            log::warning(log::category::ui, std::format("generate trainer: {}", message));
        }

        if (generated->written.empty())
        {
            status_->set_status(widgets::StatusKind::error, tr("No trainer was written"));
            return;
        }

        emit trainersGenerated(output_directory());
        if (!generated->errors.empty())
        {
            status_->set_status(
                widgets::StatusKind::warning,
                tr("Wrote %1 of the requested trainers; the rest are listed above").arg(generated->written.size()));
            return;
        }

        log::info(log::category::ui,
                  std::format(
                      "generated {} trainer(s) into {}", generated->written.size(), request.output_directory.string()));
        status_->set_status(widgets::StatusKind::success, tr("Wrote %1 trainer(s)").arg(generated->written.size()));
    }

    QString GenerateTrainerDialog::result_text() const
    {
        return results_->toPlainText();
    }

    void GenerateTrainerDialog::browse()
    {
        const auto chosen = QFileDialog::getExistingDirectory(this, tr("Output folder"), output_directory());
        if (!chosen.isEmpty())
        {
            set_output_directory(chosen);
        }
    }

    void GenerateTrainerDialog::update_state()
    {
        generate_button_->setEnabled(can_generate());
        summary_->setText(tr("%1; %2-byte pointers, target \"%3\"")
                              .arg(summary_text(),
                                   QString::number(request_.pointer_size),
                                   request_.process_name.empty() ? tr("(none recorded)")
                                                                 : QString::fromStdString(request_.process_name)));
    }

}
