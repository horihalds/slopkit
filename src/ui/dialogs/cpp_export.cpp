#include "ui/dialogs/cpp_export.hpp"

#include <format>
#include <utility>

#include <QClipboard>
#include <QFile>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"

namespace slopkit::ui::dialogs
{
    CppExportDialog::CppExportDialog(QString text, QWidget* parent) : QDialog(parent)
    {
        setWindowTitle(tr("Export C++ Header"));
        resize(780, 560);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        layout->addWidget(widgets::section_header(tr("Export C++ Header"), this));

        const int characters = text.size();

        view_ = new QPlainTextEdit(this);
        view_->setObjectName(QStringLiteral("cpp_export_view"));
        view_->setReadOnly(true);
        view_->setFont(mono_font());
        view_->setLineWrapMode(QPlainTextEdit::NoWrap);
        view_->setPlainText(std::move(text));
        layout->addWidget(view_, 1);

        auto* footer = new QHBoxLayout();

        status_ = new widgets::StatusLabel(this);
        status_->setObjectName(QStringLiteral("cpp_export_status"));
        status_->set_status(widgets::StatusKind::info, tr("%1 characters").arg(characters));
        footer->addWidget(status_, 1);

        auto* copy_button  = widgets::secondary_button(tr("Copy"), this);
        auto* save_button  = widgets::secondary_button(tr("Save As…"), this);
        auto* close_button = widgets::secondary_button(tr("Close"), this);
        copy_button->setObjectName(QStringLiteral("cpp_export_copy"));
        save_button->setObjectName(QStringLiteral("cpp_export_save"));
        close_button->setObjectName(QStringLiteral("cpp_export_close"));
        footer->addWidget(copy_button);
        footer->addWidget(save_button);
        footer->addWidget(close_button);
        layout->addLayout(footer);

        connect(copy_button, &QPushButton::clicked, this, &CppExportDialog::copy);
        connect(save_button, &QPushButton::clicked, this, &CppExportDialog::save_as);
        connect(close_button, &QPushButton::clicked, this, &QDialog::accept);
    }

    void CppExportDialog::set_dialog_directory(const QString& directory)
    {
        dialog_directory_ = directory;
    }

    QString CppExportDialog::dialog_directory() const
    {
        return dialog_directory_;
    }

    void CppExportDialog::copy()
    {
        QGuiApplication::clipboard()->setText(view_->toPlainText());
        status_->set_status(widgets::StatusKind::success, tr("Copied to the clipboard."));
    }

    void CppExportDialog::save_as()
    {
        log::debug(log::category::ui, "C++ header save as requested");
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Export C++ Header"), dialog_directory_, tr("C++ headers (*.h);;All files (*)"));
        if (path.isEmpty())
        {
            return;
        }
        save_to(path);
    }

    void CppExportDialog::save_to(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            log::warning(log::category::ui, std::format("C++ header could not be written to {}", path.toStdString()));
            status_->set_status(widgets::StatusKind::error, tr("Could not write %1").arg(path));
            return;
        }

        file.write(view_->toPlainText().toUtf8());
        log::debug(log::category::ui, std::format("C++ header saved to {}", path.toStdString()));
        status_->set_status(widgets::StatusKind::success, tr("Saved %1").arg(path));
        emit headerSaved(path);
    }

}
