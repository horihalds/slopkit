#include "trainer/runtime/gui/log_pane.hpp"

#include <QScrollBar>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>

#include "trainer/runtime/gui/log_feed.hpp"
#include "ui/fonts.hpp"
#include "ui/theme.hpp"

namespace slopkit::trainer::gui
{
    LogPane::LogPane(QWidget* parent) : QPlainTextEdit(parent)
    {
        setReadOnly(true);
        setLineWrapMode(QPlainTextEdit::NoWrap);
        setFont(ui::mono_font());
        setMaximumBlockCount(500);
    }

    void LogPane::append_records(std::span<const log::Record> records)
    {
        if (records.empty())
        {
            return;
        }

        auto* const bar    = verticalScrollBar();
        const bool  follow = bar->value() == bar->maximum();

        QTextCursor cursor(document());
        cursor.movePosition(QTextCursor::End);
        for (const auto& record : records)
        {
            QTextCharFormat format;
            if (record.level == log::Level::debug)
            {
                format.setForeground(ui::active_theme().text_muted);
            }
            else if (record.level >= log::Level::warning)
            {
                format.setForeground(ui::active_theme().error);
            }
            cursor.insertText(QString::fromStdString(log_line(record)) + QLatin1Char {'\n'}, format);
        }

        if (follow)
        {
            bar->setValue(bar->maximum());
        }
    }
}
