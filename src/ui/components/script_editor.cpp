#include "ui/components/script_editor.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include <QEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QHideEvent>
#include <QKeyEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFormat>

#include "script/api_catalog.hpp"
#include "ui/components/script_completion.hpp"
#include "ui/components/script_context.hpp"
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

        // The opening size: at least this many mono columns, growing with the
        // document's longest line up to the cap, and this many text lines tall.
        constexpr int kMinColumns     = 60;
        constexpr int kDefaultColumns = 100;
        constexpr int kMaxColumns     = 140;
        constexpr int kPreferredLines = 18;
        constexpr int kMinLines       = 12;

        [[nodiscard]] bool is_bracket(QChar ch)
        {
            return ch == QLatin1Char('(') || ch == QLatin1Char(')') || ch == QLatin1Char('[') || ch == QLatin1Char(']')
                || ch == QLatin1Char('{') || ch == QLatin1Char('}');
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

        // True when `trimmed` starts with a keyword that closes a block, or
        // with the closing half of a bracket pair, and so should resume one stop
        // further out.
        [[nodiscard]] bool starts_closing_token(const QString& trimmed)
        {
            if (!trimmed.isEmpty()
                && (trimmed.at(0) == QLatin1Char('}') || trimmed.at(0) == QLatin1Char(')')
                    || trimmed.at(0) == QLatin1Char(']')))
            {
                return true;
            }
            int n = 0;
            while (n < trimmed.size() && trimmed.at(n).isLetter())
            {
                ++n;
            }
            const QString word = trimmed.left(n);
            return word == QLatin1String("end") || word == QLatin1String("else") || word == QLatin1String("elseif")
                || word == QLatin1String("until");
        }

        // `line` up to a trailing `--` comment, trimmed: the part that decides
        // whether the line opens a block.
        [[nodiscard]] QString code_part(const QString& line)
        {
            const int comment = line.indexOf(QLatin1String("--"));
            return (comment < 0 ? line : line.left(comment)).trimmed();
        }

        // True when `line` opens a block the following line belongs to: a keyword
        // that wants a matching `end`/`until`, or a bracket left open at the
        // line's end. The next line then indents one stop further in.
        [[nodiscard]] bool opens_block(const QString& line)
        {
            const QString code = code_part(line);
            if (code.isEmpty())
            {
                return false;
            }
            const QChar last = code.at(code.size() - 1);
            if (last == QLatin1Char('{') || last == QLatin1Char('(') || last == QLatin1Char('['))
            {
                return true;
            }
            int n = code.size();
            while (n > 0 && code.at(n - 1).isLetter())
            {
                --n;
            }
            const QString word = code.mid(n);
            return word == QLatin1String("then") || word == QLatin1String("do") || word == QLatin1String("function")
                || word == QLatin1String("else") || word == QLatin1String("repeat");
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
        setTabStopDistance(QFontMetricsF(font()).horizontalAdvance(QLatin1Char(' ')) * kIndentSize);

        gutter_      = new ScriptEditorGutter(this);
        highlighter_ = new ScriptHighlighter(document());
        completion_  = new ScriptCompletionPopup(this);
        tip_         = new ScriptCallTip(this);

        connect(this,
                &QPlainTextEdit::textChanged,
                this,
                [this]
                {
                    // The longest line and so the size hint follow the text; the
                    // floor stays put, so an already-shown window is not resized.
                    longest_columns_.reset();
                    updateGeometry();
                    update_completion(true);
                    refresh_hints();
                });
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
                    update_completion(false);
                    refresh_hints();
                });
        connect(verticalScrollBar(),
                &QScrollBar::valueChanged,
                this,
                [this]
                {
                    completion_->hide_popup();
                    tip_->hide_tip();
                });
        connect(horizontalScrollBar(),
                &QScrollBar::valueChanged,
                this,
                [this]
                {
                    completion_->hide_popup();
                    tip_->hide_tip();
                });

        update_gutter_width();
        update_bands();
        setMinimumSize(minimumSizeHint());
    }

    QSize ScriptEditor::sizeHint() const
    {
        return size_for_lines(std::clamp(longest_line_columns(), kDefaultColumns, kMaxColumns), kPreferredLines);
    }

    QSize ScriptEditor::minimumSizeHint() const
    {
        return size_for_lines(kMinColumns, kMinLines);
    }

    QSize ScriptEditor::size_for_lines(int columns, int lines) const
    {
        const int width  = columns * column_width() + gutter_width_ + content_margin() + 2 * frameWidth();
        const int height = lines * line_height() + content_margin() + 2 * frameWidth();
        return QSize(width, height);
    }

    int ScriptEditor::longest_line_columns() const
    {
        if (!longest_columns_.has_value())
        {
            int longest = 0;
            for (QTextBlock block = document()->begin(); block.isValid(); block = block.next())
            {
                int columns = 0;
                for (const QChar character : block.text())
                {
                    columns += character == QLatin1Char('\t') ? kIndentSize - (columns % kIndentSize) : 1;
                }
                longest = std::max(longest, columns);
            }
            longest_columns_ = longest;
        }
        return *longest_columns_;
    }

    int ScriptEditor::column_width() const
    {
        return static_cast<int>(std::ceil(QFontMetricsF(font()).horizontalAdvance(QLatin1Char(' '))));
    }

    int ScriptEditor::line_height() const
    {
        return static_cast<int>(std::ceil(QFontMetricsF(font()).lineSpacing()));
    }

    int ScriptEditor::content_margin() const
    {
        return static_cast<int>(std::ceil(document()->documentMargin())) * 2;
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
            const int mate = script_context::matching_bracket(plain, bracket);
            if (mate >= 0)
            {
                selections.append(bracket_selection(document(), bracket, theme));
                selections.append(bracket_selection(document(), mate, theme));
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
        // An open completion list owns the navigation and accept keys; `Esc`
        // closes it and leaves the text (and the dialog) untouched.
        if (completion_->is_open())
        {
            if (event->key() == Qt::Key_Escape)
            {
                completion_->hide_popup();
                event->accept();
                return;
            }
            if (event->key() == Qt::Key_Up)
            {
                completion_->move_selection(-1);
                event->accept();
                return;
            }
            if (event->key() == Qt::Key_Down)
            {
                completion_->move_selection(1);
                event->accept();
                return;
            }
            if (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            {
                accept_completion();
                event->accept();
                return;
            }
        }

        if (event->key() == Qt::Key_Space && (event->modifiers() & Qt::ControlModifier))
        {
            open_completion();
            event->accept();
            return;
        }

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

        // Backspace that follows four or more spaces takes the whole stop at
        // once, so the caret walks the indentation a stop at a time.
        if (event->key() == Qt::Key_Backspace && event->modifiers() == Qt::NoModifier)
        {
            QTextCursor   cursor = textCursor();
            const QString line   = cursor.block().text();
            const int     column = cursor.positionInBlock();
            if (!cursor.hasSelection() && column >= kIndentSize
                && line.mid(column - kIndentSize, kIndentSize) == QString(kIndentSize, QLatin1Char(' ')))
            {
                cursor.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, kIndentSize);
                cursor.removeSelectedText();
                setTextCursor(cursor);
                event->accept();
                return;
            }
        }

        QPlainTextEdit::keyPressEvent(event);
    }

    void ScriptEditor::focusOutEvent(QFocusEvent* event)
    {
        completion_->hide_popup();
        tip_->hide_tip();
        QPlainTextEdit::focusOutEvent(event);
    }

    void ScriptEditor::hideEvent(QHideEvent* event)
    {
        completion_->hide_popup();
        tip_->hide_tip();
        QPlainTextEdit::hideEvent(event);
    }

    QRect ScriptEditor::caret_rect() const
    {
        QRect rect = cursorRect();
        rect.setWidth(std::max(rect.width(), 1));
        return rect;
    }

    void ScriptEditor::update_completion(bool allow_open)
    {
        const QString text     = toPlainText();
        const int     position = textCursor().position();

        const std::optional<script_context::Identifier> identifier = script_context::identifier_at(text, position);
        const QString                                   receiver   = identifier ? identifier->receiver : QString {};
        const QString prefix = identifier ? text.mid(identifier->begin, position - identifier->begin) : QString {};

        // Only a caret in code that touches a word (or follows a dot) can open
        // the list; anything else closes it.
        if (identifier && (!prefix.isEmpty() || !receiver.isEmpty()))
        {
            if (completion_->is_open() || allow_open)
            {
                completion_->show_at(caret_rect(), receiver, prefix);
            }
            return;
        }
        completion_->hide_popup();
    }

    void ScriptEditor::open_completion()
    {
        const QString                                   text       = toPlainText();
        const int                                       position   = textCursor().position();
        const std::optional<script_context::Identifier> identifier = script_context::identifier_at(text, position);
        const QString                                   receiver   = identifier ? identifier->receiver : QString {};
        const QString prefix = identifier ? text.mid(identifier->begin, position - identifier->begin) : QString {};
        completion_->show_at(caret_rect(), receiver, prefix);
        refresh_hints(); // the open list supersedes the hint
    }

    void ScriptEditor::accept_completion()
    {
        const std::optional<QString> name        = completion_->selected_name();
        const bool                   is_function = completion_->selected_is_function();
        if (!name.has_value())
        {
            completion_->hide_popup();
            return;
        }

        const QString                                   text       = toPlainText();
        const int                                       position   = textCursor().position();
        const std::optional<script_context::Identifier> identifier = script_context::identifier_at(text, position);
        if (!identifier.has_value())
        {
            completion_->hide_popup();
            return;
        }

        QTextCursor cursor = textCursor();
        // One edit block, so a single Ctrl+Z undoes the name and the inserted
        // parentheses together.
        cursor.beginEditBlock();
        cursor.setPosition(identifier->begin);
        cursor.setPosition(identifier->end, QTextCursor::KeepAnchor);
        cursor.insertText(*name);
        if (is_function && document()->characterAt(cursor.position()) != QLatin1Char('('))
        {
            cursor.insertText(QStringLiteral("()"));
            cursor.movePosition(QTextCursor::Left);
        }
        cursor.endEditBlock();

        setTextCursor(cursor);
        completion_->hide_popup();
    }

    void ScriptEditor::refresh_hints()
    {
        if (completion_->is_open())
        {
            tip_->hide_tip();
            return;
        }

        const QString                             text     = toPlainText();
        const int                                 position = textCursor().position();
        const std::optional<script_context::Call> call     = script_context::enclosing_call(text, position);
        if (!call.has_value())
        {
            tip_->hide_tip();
            return;
        }

        // A catalogue function wins (a dotted `mem.read` or a plain global).
        const QString full = call->receiver.isEmpty() ? call->name : call->receiver + QLatin1Char('.') + call->name;
        if (const script::ApiEntry* entry = script::find(full.toStdString()))
        {
            tip_->show_at(caret_rect(),
                          QString::fromUtf8(entry->signature.data(), static_cast<int>(entry->signature.size())),
                          script::parameter_index(entry->signature, static_cast<std::size_t>(call->active_parameter)),
                          QString::fromUtf8(entry->summary.data(), static_cast<int>(entry->summary.size())));
            return;
        }

        // A function the document defines shows its own parameter list.
        for (const script_context::DocumentName& name : script_context::document_names(text))
        {
            if (name.function && name.name == call->name)
            {
                tip_->show_at(
                    caret_rect(), name.signature, static_cast<std::size_t>(call->active_parameter), QString {});
                return;
            }
        }
        tip_->hide_tip();
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

        // A line that opens with a closing keyword or bracket resumes one stop
        // further out; one whose text before the caret leaves a block open hands
        // one stop further in to the line it starts.
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
        else if (opens_block(line.left(cursor.positionInBlock())))
        {
            indent += QString(kIndentSize, QLatin1Char(' '));
        }

        cursor.beginEditBlock();
        cursor.insertText(QStringLiteral("\n") + indent);
        cursor.endEditBlock();
        setTextCursor(cursor);
    }

} // namespace slopkit::ui::components
