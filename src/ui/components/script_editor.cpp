#include "ui/components/script_editor.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include <QEvent>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFormat>

#include "ui/components/script_highlighter.hpp"
#include "ui/fonts.hpp"
#include "ui/theme.hpp"

namespace slopkit::ui::components
{

    namespace
    {
        // The indentation of one tab stop: four spaces, matching the script
        // dialog's own skeleton.
        constexpr int kIndentSize = 4;

        [[nodiscard]] bool is_bracket(QChar ch)
        {
            return ch == QLatin1Char('(') || ch == QLatin1Char(')') || ch == QLatin1Char('[') || ch == QLatin1Char(']')
                || ch == QLatin1Char('{') || ch == QLatin1Char('}');
        }

        // Matches a long-bracket opener `[=*[` at `index`; returns the offset
        // just past the second bracket and reports the `=` count, or -1.
        [[nodiscard]] int long_bracket_open(const QString& text, int index, int* level)
        {
            if (index >= text.size() || text.at(index) != QLatin1Char('['))
            {
                return -1;
            }
            int i  = index + 1;
            int eq = 0;
            while (i < text.size() && text.at(i) == QLatin1Char('='))
            {
                ++eq;
                ++i;
            }
            if (i < text.size() && text.at(i) == QLatin1Char('['))
            {
                *level = eq;
                return i + 1;
            }
            return -1;
        }

        // The offset just past the matching `]=*]`, or -1 when none follows.
        [[nodiscard]] int long_bracket_close(const QString& text, int from, int level)
        {
            const QString close = QStringLiteral("]") + QString(level, QLatin1Char('=')) + QStringLiteral("]");
            const int     at    = text.indexOf(close, from);
            return at < 0 ? -1 : at + close.size();
        }

        // The offset just past a short string starting at the quote `index`.
        [[nodiscard]] int short_string_end(const QString& text, int index)
        {
            const QChar quote = text.at(index);
            int         i     = index + 1;
            while (i < text.size())
            {
                const QChar ch = text.at(i);
                if (ch == QLatin1Char('\\'))
                {
                    i += 2;
                    continue;
                }
                if (ch == quote)
                {
                    return i + 1;
                }
                ++i;
            }
            return text.size();
        }

        // Marks every character that lies outside a comment or a string, so
        // bracket matching can ignore the ones inside them.
        [[nodiscard]] std::vector<bool> code_mask(const QString& text)
        {
            std::vector<bool> mask(static_cast<std::size_t>(text.size()), true);

            int i = 0;
            while (i < text.size())
            {
                const QChar ch = text.at(i);

                if (ch == QLatin1Char('-') && i + 1 < text.size() && text.at(i + 1) == QLatin1Char('-'))
                {
                    int       level = 0;
                    const int open  = long_bracket_open(text, i + 2, &level);
                    int       end   = text.size();
                    if (open >= 0)
                    {
                        const int close = long_bracket_close(text, open, level);
                        end             = close < 0 ? text.size() : close;
                    }
                    else
                    {
                        end = i;
                        while (end < text.size() && text.at(end) != QLatin1Char('\n'))
                        {
                            ++end;
                        }
                    }
                    for (int j = i; j < end; ++j)
                    {
                        mask[static_cast<std::size_t>(j)] = false;
                    }
                    i = end;
                    continue;
                }

                if (ch == QLatin1Char('['))
                {
                    int       level = 0;
                    const int open  = long_bracket_open(text, i, &level);
                    if (open >= 0)
                    {
                        const int close = long_bracket_close(text, open, level);
                        const int end   = close < 0 ? text.size() : close;
                        for (int j = i; j < end; ++j)
                        {
                            mask[static_cast<std::size_t>(j)] = false;
                        }
                        i = end;
                        continue;
                    }
                }

                if (ch == QLatin1Char('"') || ch == QLatin1Char('\''))
                {
                    const int end = short_string_end(text, i);
                    for (int j = i; j < end; ++j)
                    {
                        mask[static_cast<std::size_t>(j)] = false;
                    }
                    i = end;
                    continue;
                }

                ++i;
            }

            return mask;
        }

        // The position of the bracket matching the one at `index`, skipping
        // brackets inside comments and strings, or -1 when there is none.
        [[nodiscard]] int matching_bracket(const QString& text, const std::vector<bool>& code, int index)
        {
            const QChar ch = text.at(index);

            const std::pair<QChar, QChar> pairs[] = {
                {QLatin1Char('('), QLatin1Char(')')},
                {QLatin1Char('['), QLatin1Char(']')},
                {QLatin1Char('{'), QLatin1Char('}')}
            };
            bool  opening = false;
            QChar open {0};
            QChar close {0};
            for (const auto& [first, second] : pairs)
            {
                if (ch == first)
                {
                    opening = true;
                    open    = first;
                    close   = second;
                    break;
                }
                if (ch == second)
                {
                    opening = false;
                    open    = first;
                    close   = second;
                    break;
                }
            }
            if (open == QChar {0})
            {
                return -1;
            }

            if (opening)
            {
                int depth = 0;
                for (int i = index; i < text.size(); ++i)
                {
                    if (!code[static_cast<std::size_t>(i)])
                    {
                        continue;
                    }
                    if (text.at(i) == open)
                    {
                        ++depth;
                    }
                    else if (text.at(i) == close)
                    {
                        --depth;
                        if (depth == 0)
                        {
                            return i;
                        }
                    }
                }
            }
            else
            {
                int depth = 0;
                for (int i = index; i >= 0; --i)
                {
                    if (!code[static_cast<std::size_t>(i)])
                    {
                        continue;
                    }
                    if (text.at(i) == close)
                    {
                        ++depth;
                    }
                    else if (text.at(i) == open)
                    {
                        --depth;
                        if (depth == 0)
                        {
                            return i;
                        }
                    }
                }
            }

            return -1;
        }

        // An extra selection that paints the single character at `position` in
        // the accent colour.
        [[nodiscard]] QTextEdit::ExtraSelection
        bracket_selection(QTextDocument* document, int position, const Theme& theme)
        {
            QTextEdit::ExtraSelection selection;
            selection.format.setBackground(theme.accent);
            selection.format.setForeground(theme.on_accent);
            QTextCursor cursor(document);
            cursor.setPosition(position);
            cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 1);
            selection.cursor = cursor;
            return selection;
        }

        // True when `trimmed` starts with a keyword that closes a block and so
        // should resume one stop further out.
        [[nodiscard]] bool starts_closing_token(const QString& trimmed)
        {
            int n = 0;
            while (n < trimmed.size() && trimmed.at(n).isLetter())
            {
                ++n;
            }
            const QString word = trimmed.left(n);
            return word == QLatin1String("end") || word == QLatin1String("else") || word == QLatin1String("elseif")
                || word == QLatin1String("until");
        }
    } // namespace

    ScriptEditorGutter::ScriptEditorGutter(ScriptEditor* editor) : QWidget(editor), editor_(editor) {}

    QSize ScriptEditorGutter::sizeHint() const
    {
        return QSize(editor_->gutter_width(), 0);
    }

    void ScriptEditorGutter::paintEvent(QPaintEvent* event)
    {
        editor_->paint_gutter(event);
    }

    ScriptEditor::ScriptEditor(QWidget* parent) : QPlainTextEdit(parent)
    {
        setFont(mono_font());
        setLineWrapMode(QPlainTextEdit::NoWrap);
        setMinimumHeight(240);
        setTabStopDistance(QFontMetricsF(font()).horizontalAdvance(QLatin1Char(' ')) * kIndentSize);

        gutter_      = new ScriptEditorGutter(this);
        highlighter_ = new ScriptHighlighter(document());

        connect(this,
                &QPlainTextEdit::blockCountChanged,
                this,
                [this](int)
                {
                    update_gutter_width();
                });
        connect(this,
                &QPlainTextEdit::updateRequest,
                this,
                [this](const QRect& rect, int dy)
                {
                    if (dy != 0)
                    {
                        gutter_->scroll(0, dy);
                    }
                    else
                    {
                        gutter_->update(0, rect.y(), gutter_->width(), rect.height());
                    }
                });
        connect(this,
                &QPlainTextEdit::cursorPositionChanged,
                this,
                [this]
                {
                    update_bands();
                });

        update_gutter_width();
        update_bands();
    }

    void ScriptEditor::set_error_line(int line)
    {
        if (line < 0)
        {
            return;
        }
        if (line > 0 && line > blockCount())
        {
            return; // a line outside the document is ignored
        }

        error_line_ = line;
        update_bands();
        gutter_->update();
    }

    int ScriptEditor::error_line() const noexcept
    {
        return error_line_;
    }

    int ScriptEditor::gutter_width() const noexcept
    {
        return gutter_width_;
    }

    void ScriptEditor::update_gutter_width()
    {
        const int digits = QString::number(std::max(1, blockCount())).size();
        const int digit  = static_cast<int>(std::ceil(QFontMetricsF(font()).horizontalAdvance(QLatin1Char('9'))));
        const int space  = static_cast<int>(std::ceil(QFontMetricsF(font()).horizontalAdvance(QLatin1Char(' '))));

        gutter_width_ = digit * digits + space * 2;
        setViewportMargins(gutter_width_, 0, 0, 0);
    }

    void ScriptEditor::paint_gutter(QPaintEvent* event)
    {
        QPainter     painter(gutter_);
        const Theme& theme = active_theme();
        painter.fillRect(event->rect(), theme.surface);

        const int caret_line = textCursor().blockNumber();
        const int right_pad  = static_cast<int>(std::ceil(QFontMetricsF(font()).horizontalAdvance(QLatin1Char(' '))));

        QTextBlock block        = firstVisibleBlock();
        int        block_number = block.blockNumber();
        int        top          = static_cast<int>(blockBoundingGeometry(block).translated(contentOffset()).top());
        int        bottom       = top + static_cast<int>(blockBoundingRect(block).height());

        while (block.isValid() && top <= event->rect().bottom())
        {
            if (block.isVisible() && bottom >= event->rect().top())
            {
                const int line = block_number + 1;

                QFont number_font = font();
                number_font.setBold(line == error_line_);
                painter.setFont(number_font);

                if (line == error_line_)
                {
                    painter.setPen(theme.error);
                }
                else if (block_number == caret_line)
                {
                    painter.setPen(theme.text);
                }
                else
                {
                    painter.setPen(theme.text_muted);
                }

                const int height = static_cast<int>(std::ceil(QFontMetricsF(number_font).height()));
                painter.drawText(0, top, gutter_->width() - right_pad, height, Qt::AlignRight, QString::number(line));
            }

            block  = block.next();
            top    = bottom;
            bottom = top + static_cast<int>(blockBoundingRect(block).height());
            ++block_number;
        }
    }

    void ScriptEditor::update_bands()
    {
        const Theme&                     theme = active_theme();
        QList<QTextEdit::ExtraSelection> selections;

        QTextEdit::ExtraSelection current;
        current.format.setBackground(theme.surface_hover);
        current.format.setProperty(QTextFormat::FullWidthSelection, true);
        current.cursor = textCursor();
        current.cursor.clearSelection();
        selections.append(current);

        if (error_line_ > 0 && error_line_ <= blockCount())
        {
            QColor band = theme.error;
            band.setAlpha(64);

            QTextEdit::ExtraSelection error;
            error.format.setBackground(band);
            error.format.setProperty(QTextFormat::FullWidthSelection, true);
            error.cursor = QTextCursor(document()->findBlockByNumber(error_line_ - 1));
            error.cursor.clearSelection();
            selections.append(error);
        }

        const QString plain    = toPlainText();
        const int     position = textCursor().position();
        int           bracket  = -1;
        if (position < plain.size() && is_bracket(plain.at(position)))
        {
            bracket = position;
        }
        else if (position > 0 && is_bracket(plain.at(position - 1)))
        {
            bracket = position - 1;
        }

        if (bracket >= 0)
        {
            const std::vector<bool> code = code_mask(plain);
            if (bracket < static_cast<int>(code.size()) && code[static_cast<std::size_t>(bracket)])
            {
                const int mate = matching_bracket(plain, code, bracket);
                if (mate >= 0)
                {
                    selections.append(bracket_selection(document(), bracket, theme));
                    selections.append(bracket_selection(document(), mate, theme));
                }
            }
        }

        setExtraSelections(selections);
    }

    void ScriptEditor::resizeEvent(QResizeEvent* event)
    {
        QPlainTextEdit::resizeEvent(event);

        const QRect area = contentsRect();
        gutter_->setGeometry(QRect(area.left(), area.top(), gutter_width_, area.height()));
    }

    void ScriptEditor::changeEvent(QEvent* event)
    {
        QPlainTextEdit::changeEvent(event);

        if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        {
            highlighter_->rehighlight();
            update_bands();
            gutter_->update();
        }
    }

    void ScriptEditor::keyPressEvent(QKeyEvent* event)
    {
        if (event->key() == Qt::Key_Backtab
            || (event->key() == Qt::Key_Tab && (event->modifiers() & Qt::ShiftModifier)))
        {
            indent_lines(-1);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Tab)
        {
            indent_lines(1);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        {
            insert_newline();
            event->accept();
            return;
        }

        QPlainTextEdit::keyPressEvent(event);
    }

    void ScriptEditor::indent_lines(int direction)
    {
        QTextCursor cursor = textCursor();
        QTextCursor start(document());
        start.setPosition(cursor.selectionStart());
        QTextCursor end(document());
        end.setPosition(cursor.selectionEnd());

        const int first_block = start.blockNumber();
        const int last_block  = end.blockNumber();

        cursor.beginEditBlock();
        for (int number = first_block; number <= last_block; ++number)
        {
            const QTextBlock block = document()->findBlockByNumber(number);
            if (!block.isValid())
            {
                continue;
            }

            QTextCursor line(block);
            if (direction > 0)
            {
                line.insertText(QString(kIndentSize, QLatin1Char(' ')));
                continue;
            }

            int leading = 0;
            while (leading < block.text().size() && block.text().at(leading) == QLatin1Char(' '))
            {
                ++leading;
            }
            const int remove = std::min(leading, kIndentSize);
            if (remove > 0)
            {
                line.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, remove);
                line.removeSelectedText();
            }
        }
        cursor.endEditBlock();

        // Keep the indented lines selected.
        QTextCursor selection(document()->findBlockByNumber(first_block));
        selection.movePosition(QTextCursor::StartOfBlock);
        QTextCursor tail(document()->findBlockByNumber(last_block));
        tail.movePosition(QTextCursor::EndOfBlock);
        selection.setPosition(tail.position(), QTextCursor::KeepAnchor);
        setTextCursor(selection);
    }

    void ScriptEditor::insert_newline()
    {
        QTextCursor   cursor = textCursor();
        const QString line   = cursor.block().text();

        int leading = 0;
        while (leading < line.size() && (line.at(leading) == QLatin1Char(' ') || line.at(leading) == QLatin1Char('\t')))
        {
            ++leading;
        }
        QString indent = line.left(leading);

        // A line that opens with a closing keyword resumes one stop further out.
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty() && starts_closing_token(trimmed))
        {
            int spaces = 0;
            while (spaces < indent.size() && indent.at(spaces) == QLatin1Char(' '))
            {
                ++spaces;
            }
            indent = indent.left(std::max(0, spaces - kIndentSize));
        }

        cursor.beginEditBlock();
        cursor.insertText(QStringLiteral("\n") + indent);
        cursor.endEditBlock();
        setTextCursor(cursor);
    }

} // namespace slopkit::ui::components
