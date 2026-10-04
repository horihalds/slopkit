#include "ui/dialogs/log.hpp"

#include <chrono>
#include <format>
#include <string_view>
#include <utility>

#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"

namespace slopkit::ui::dialogs
{

    namespace
    {
        QString to_qstring(std::string_view text)
        {
            return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
        }

        QString record_text(const log::Record& record)
        {
            const auto since_epoch =
                std::chrono::duration_cast<std::chrono::milliseconds>(record.time.time_since_epoch());
            const QDateTime stamp = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(since_epoch.count()));
            return QStringLiteral("%1 %2 [%3] %4")
                .arg(stamp.toString(QStringLiteral("HH:mm:ss.zzz")))
                .arg(to_qstring(log::level_name(record.level)))
                .arg(to_qstring(record.category))
                .arg(to_qstring(record.message));
        }

        QString searchable_text(const log::Record& record)
        {
            return to_qstring(log::level_name(record.level)) + QLatin1Char(' ') + to_qstring(record.category)
                 + QLatin1Char(' ') + to_qstring(record.message);
        }
    } // namespace

    LogDialog::LogDialog(QWidget* parent) : QDialog(parent)
    {
        setWindowTitle(tr("Log"));
        resize(760, 520);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        layout->addWidget(widgets::section_header(tr("Log"), this));

        auto* filter_row = new QHBoxLayout();

        level_filter_ = new widgets::ScrollingComboBox(10, this);
        level_filter_->addItem(tr("All"));
        level_filter_->addItem(tr("Debug"));
        level_filter_->addItem(tr("Info"));
        level_filter_->addItem(tr("Warning"));
        level_filter_->addItem(tr("Error"));
        filter_row->addWidget(level_filter_);

        text_filter_ = new QLineEdit(this);
        text_filter_->setPlaceholderText(tr("Search"));
        text_filter_->setClearButtonEnabled(true);
        filter_row->addWidget(text_filter_, 1);

        auto* clear_button = widgets::secondary_button(tr("Clear"), this);
        auto* save_button  = widgets::secondary_button(tr("Save As..."), this);
        filter_row->addWidget(clear_button);
        filter_row->addWidget(save_button);
        layout->addLayout(filter_row);

        view_ = new QPlainTextEdit(this);
        view_->setReadOnly(true);
        view_->setFont(mono_font());
        view_->setLineWrapMode(QPlainTextEdit::NoWrap);
        layout->addWidget(view_, 1);

        status_ = new widgets::StatusLabel(this);
        status_->setObjectName(QStringLiteral("log_status"));
        layout->addWidget(status_);

        connect(level_filter_,
                &QComboBox::currentIndexChanged,
                this,
                [this]
                {
                    render();
                });
        connect(text_filter_,
                &QLineEdit::textChanged,
                this,
                [this]
                {
                    render();
                });
        connect(clear_button, &QPushButton::clicked, this, &LogDialog::clear);
        connect(save_button, &QPushButton::clicked, this, &LogDialog::save_as);
        connect(&notifier_, &LogNotifier::recordsAvailable, this, &LogDialog::drain);

        render();
    }

    void LogDialog::showEvent(QShowEvent* event)
    {
        QDialog::showEvent(event);
        if (seeded_)
        {
            return;
        }
        seeded_ = true;

        records_ = log::Logger::instance().history();

        // Merge records delivered before the snapshot without duplicating the ones
        // already in the history; anything newer than the last history entry is
        // appended.
        const auto newest = records_.empty() ? std::chrono::system_clock::time_point {} : records_.back().time;
        for (auto& record : notifier_.take())
        {
            if (records_.empty() || record.time > newest)
            {
                records_.push_back(std::move(record));
            }
        }
        render();
    }

    void LogDialog::drain()
    {
        for (auto& record : notifier_.take())
        {
            records_.push_back(std::move(record));
        }
        render();
    }

    void LogDialog::render()
    {
        // Combo rows map to the minimum level: All/Debug show everything, then
        // Info, Warning and Error narrow it.
        const int     minimum_rank = level_filter_->currentIndex() <= 0 ? 0 : level_filter_->currentIndex() - 1;
        const QString needle       = text_filter_->text();

        QString text;
        int     shown = 0;
        for (const auto& record : records_)
        {
            if (static_cast<int>(record.level) < minimum_rank)
            {
                continue;
            }
            if (!needle.isEmpty() && !searchable_text(record).contains(needle, Qt::CaseInsensitive))
            {
                continue;
            }
            text += record_text(record);
            text += QLatin1Char('\n');
            ++shown;
        }

        view_->setPlainText(text);
        view_->verticalScrollBar()->setValue(view_->verticalScrollBar()->maximum());
        update_status(shown);
    }

    void LogDialog::clear()
    {
        records_.clear();
        notifier_.take();
        log::Logger::instance().clear_history();
        render();
    }

    void LogDialog::set_dialog_directory(const QString& directory)
    {
        dialog_directory_ = directory;
    }

    void LogDialog::save_as()
    {
        log::debug(log::category::ui, "log save as requested");
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Save Log"), dialog_directory_, tr("Log files (*.log);;All files (*)"));
        if (path.isEmpty())
        {
            return;
        }
        save_to(path);
    }

    void LogDialog::save_to(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            log::warning(log::category::ui, std::format("log could not be written to {}", path.toStdString()));
            status_->set_status(widgets::StatusKind::error, tr("Could not write %1").arg(path));
            return;
        }
        file.write(view_->toPlainText().toUtf8());
        log::debug(log::category::ui, std::format("log saved to {}", path.toStdString()));
        status_->set_status(widgets::StatusKind::success, tr("Saved %1").arg(path));
        emit logSaved(path);
    }

    void LogDialog::update_status(int shown)
    {
        const QString path    = QString::fromStdString(log::default_log_path().string());
        QString       message = tr("%1 record(s)").arg(shown);
        if (shown != static_cast<int>(records_.size()))
        {
            message += tr(" of %1").arg(records_.size());
        }
        message += QStringLiteral(" - ") + path;
        status_->set_status(shown == 0 ? widgets::StatusKind::warning : widgets::StatusKind::info, message);
    }

} // namespace slopkit::ui::dialogs
