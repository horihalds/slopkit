#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

#include "ui/components/script_editor.hpp"

#include <QCoreApplication>
#include <QKeyEvent>
#include <QTextCursor>
#include <QTextEdit>

namespace
{
    using slopkit::ui::components::ScriptEditor;

    void
    press(ScriptEditor& editor, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier, const QString& text = {})
    {
        QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
        QCoreApplication::sendEvent(&editor, &event);
    }

    // The number of extra selections painted with the accent colour, i.e. the
    // matched-bracket highlights.
    [[nodiscard]] int accent_selections(const ScriptEditor& editor)
    {
        const QColor accent = slopkit::ui::active_theme().accent;
        int          count  = 0;
        for (const QTextEdit::ExtraSelection& selection : editor.extraSelections())
        {
            if (selection.format.background().color() == accent)
            {
                ++count;
            }
        }
        return count;
    }
} // namespace

TEST_CASE("the script editor grows its gutter with the line count", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    ScriptEditor editor;
    editor.setPlainText(QStringLiteral("one line"));
    const int one_digit = editor.gutter_width();

    QString many;
    for (int i = 0; i < 12; ++i)
    {
        many += QStringLiteral("line\n");
    }
    editor.setPlainText(many);
    const int two_digits = editor.gutter_width();
    CHECK(two_digits > one_digit);

    editor.setPlainText(QStringLiteral("one line"));
    CHECK(editor.gutter_width() == one_digit);
}

TEST_CASE("the script editor marks and clears an error line", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    ScriptEditor editor;
    editor.setPlainText(QStringLiteral("a\nb\nc\nd"));

    editor.set_error_line(3);
    CHECK(editor.error_line() == 3);

    editor.set_error_line(0);
    CHECK(editor.error_line() == 0);

    editor.set_error_line(999); // outside the document: ignored
    CHECK(editor.error_line() == 0);

    editor.set_error_line(-1); // ignored
    CHECK(editor.error_line() == 0);

    editor.set_error_line(2);
    CHECK(editor.error_line() == 2);
}

TEST_CASE("the script editor indents with tab stops and keeps indentation", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    ScriptEditor editor;
    editor.setPlainText(QStringLiteral("local x"));

    QTextCursor start = editor.textCursor();
    start.movePosition(QTextCursor::Start);
    editor.setTextCursor(start);

    press(editor, Qt::Key_Tab);
    CHECK(editor.toPlainText() == QStringLiteral("    local x"));

    press(editor, Qt::Key_Backtab, Qt::ShiftModifier);
    CHECK(editor.toPlainText() == QStringLiteral("local x"));

    // Typing a line then Return keeps the line's indentation.
    editor.setPlainText(QStringLiteral("    local x"));
    QTextCursor end = editor.textCursor();
    end.movePosition(QTextCursor::End);
    editor.setTextCursor(end);
    press(editor, Qt::Key_Return);

    CHECK(editor.toPlainText() == QStringLiteral("    local x\n    "));
}

TEST_CASE("the script editor highlights a matched bracket at the caret", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    ScriptEditor editor;
    editor.setPlainText(QStringLiteral("("));
    QTextCursor end = editor.textCursor();
    end.movePosition(QTextCursor::End);
    editor.setTextCursor(end);

    press(editor, Qt::Key_ParenRight, Qt::ShiftModifier, QStringLiteral(")"));
    CHECK(editor.toPlainText() == QStringLiteral("()"));
    CHECK(accent_selections(editor) == 2);

    editor.setPlainText(QStringLiteral("() ab"));
    QTextCursor away = editor.textCursor();
    away.movePosition(QTextCursor::End);
    editor.setTextCursor(away);
    CHECK(accent_selections(editor) == 0);
}
