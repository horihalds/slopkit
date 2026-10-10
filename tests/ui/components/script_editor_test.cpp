#include <catch2/catch.hpp>

#include <cmath>

#include "support/ui_helpers.hpp"

#include "ui/components/script_completion.hpp"
#include "ui/components/script_editor.hpp"

#include <QCoreApplication>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>

namespace
{
    using slopkit::ui::components::ScriptCallTip;
    using slopkit::ui::components::ScriptCompletionPopup;
    using slopkit::ui::components::ScriptEditor;

    void
    press(ScriptEditor& editor, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier, const QString& text = {})
    {
        QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
        QCoreApplication::sendEvent(&editor, &event);
    }

    // Types `text` one character at a time, as a user would, so the editor's own
    // key handling and the completion trigger run.
    void type(ScriptEditor& editor, const QString& text)
    {
        for (const QChar character : text)
        {
            press(editor, character.unicode(), Qt::NoModifier, QString(character));
        }
    }

    // Puts the caret at the end of the text without triggering a key press.
    void move_to_end(ScriptEditor& editor)
    {
        QTextCursor cursor = editor.textCursor();
        cursor.movePosition(QTextCursor::End);
        editor.setTextCursor(cursor);
    }

    [[nodiscard]] ScriptCompletionPopup* completion_of(ScriptEditor& editor)
    {
        return editor.findChild<ScriptCompletionPopup*>();
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

TEST_CASE("the script editor size hint follows the longest line, its minimum does not", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    ScriptEditor editor;
    const int    column = static_cast<int>(std::ceil(QFontMetricsF(editor.font()).horizontalAdvance(QLatin1Char(' '))));
    const int    line   = static_cast<int>(std::ceil(QFontMetricsF(editor.font()).lineSpacing()));

    const int gutter   = editor.gutter_width();
    const int margin   = static_cast<int>(std::ceil(editor.document()->documentMargin()));
    const int frame    = margin * 2 + 2 * editor.frameWidth();
    const int overhead = gutter + frame;

    // A short document opens at the 100-column floor and 18 lines.
    editor.setPlainText(QStringLiteral("return true\n"));
    const QSize short_hint = editor.sizeHint();
    CHECK(short_hint.width() == 100 * column + overhead);
    CHECK(short_hint.height() == 18 * line + frame);

    // A long line widens the hint, up to the 140-column cap and no further.
    editor.setPlainText(QString(300, QLatin1Char('x')));
    const int capped = editor.sizeHint().width();
    CHECK(capped == 140 * column + overhead);

    editor.setPlainText(QString(600, QLatin1Char('x')));
    CHECK(editor.sizeHint().width() == capped); // the cap holds

    // The minimum is a fixed 60x12 and never follows the text.
    const QSize minimum = editor.minimumSizeHint();
    CHECK(minimum.width() == 60 * column + overhead);
    CHECK(minimum.height() == 12 * line + frame);

    editor.setPlainText(QString(600, QLatin1Char('x')));
    CHECK(editor.minimumSizeHint() == minimum);
}

TEST_CASE("typing opens the completion list and Tab inserts the name", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    ScriptEditor           editor;
    ScriptCompletionPopup* completion = completion_of(editor);
    REQUIRE(completion != nullptr);

    type(editor, QStringLiteral("aob"));
    REQUIRE(completion->is_open());
    REQUIRE(completion->selected_name().has_value());
    CHECK(*completion->selected_name() == QStringLiteral("aobscan"));

    press(editor, Qt::Key_Tab);
    CHECK(editor.toPlainText() == QStringLiteral("aobscan()"));
    CHECK_FALSE(completion->is_open());
    CHECK(editor.textCursor().position() == 8); // between the parentheses
}

TEST_CASE("Ctrl+Space opens the whole list and Up/Down move the highlight", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    ScriptEditor           editor;
    ScriptCompletionPopup* completion = completion_of(editor);
    REQUIRE(completion != nullptr);

    press(editor, Qt::Key_Space, Qt::ControlModifier);
    REQUIRE(completion->is_open());
    REQUIRE(completion->selected_name().has_value());
    CHECK(*completion->selected_name() == QStringLiteral("alloc")); // first API name

    press(editor, Qt::Key_Down);
    CHECK(*completion->selected_name() == QStringLiteral("aobscan"));
    press(editor, Qt::Key_Up);
    CHECK(*completion->selected_name() == QStringLiteral("alloc"));

    press(editor, Qt::Key_Escape);
    CHECK_FALSE(completion->is_open());
}

TEST_CASE("a dotted receiver lists and completes the table members", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    ScriptEditor           editor;
    ScriptCompletionPopup* completion = completion_of(editor);
    REQUIRE(completion != nullptr);

    editor.setPlainText(QStringLiteral("mem."));
    move_to_end(editor);
    press(editor, Qt::Key_Space, Qt::ControlModifier);
    REQUIRE(completion->is_open());
    REQUIRE(completion->selected_name().has_value());
    CHECK(*completion->selected_name() == QStringLiteral("pointer_size"));

    type(editor, QStringLiteral("re"));
    REQUIRE(completion->is_open());
    REQUIRE(completion->selected_name().has_value());
    CHECK(*completion->selected_name() == QStringLiteral("read"));

    press(editor, Qt::Key_Tab);
    CHECK(editor.toPlainText() == QStringLiteral("mem.read()"));
    CHECK(editor.textCursor().position() == 9);
}

TEST_CASE("the completion list stays out of comments and strings", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    ScriptEditor           editor;
    ScriptCompletionPopup* completion = completion_of(editor);
    REQUIRE(completion != nullptr);

    editor.setPlainText(QStringLiteral("-- aob"));
    move_to_end(editor);
    type(editor, QStringLiteral("x"));
    CHECK_FALSE(completion->is_open());

    editor.setPlainText(QStringLiteral("x = \"aob\""));
    QTextCursor inside = editor.textCursor();
    inside.setPosition(8); // just inside the string, before the closing quote
    editor.setTextCursor(inside);
    type(editor, QStringLiteral("c"));
    CHECK_FALSE(completion->is_open());
}

TEST_CASE("the completion list stays closed when nothing matches", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    ScriptEditor           editor;
    ScriptCompletionPopup* completion = completion_of(editor);
    REQUIRE(completion != nullptr);

    type(editor, QStringLiteral("zzzz"));
    CHECK_FALSE(completion->is_open());
}

TEST_CASE("Esc closes the completion list and leaves the text untouched", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    ScriptEditor           editor;
    ScriptCompletionPopup* completion = completion_of(editor);
    REQUIRE(completion != nullptr);

    type(editor, QStringLiteral("aob"));
    REQUIRE(completion->is_open());
    const QString before = editor.toPlainText();

    press(editor, Qt::Key_Escape);
    CHECK_FALSE(completion->is_open());
    CHECK(editor.toPlainText() == before);
}

TEST_CASE("one undo reverts an accepted completion", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    ScriptEditor           editor;
    ScriptCompletionPopup* completion = completion_of(editor);
    REQUIRE(completion != nullptr);

    type(editor, QStringLiteral("aob"));
    REQUIRE(completion->is_open());
    press(editor, Qt::Key_Tab);
    REQUIRE(editor.toPlainText() == QStringLiteral("aobscan()"));

    editor.undo();
    CHECK(editor.toPlainText() == QStringLiteral("aob"));
}

TEST_CASE("the completion list offers the names the text defines", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    ScriptEditor           editor;
    ScriptCompletionPopup* completion = completion_of(editor);
    REQUIRE(completion != nullptr);

    editor.setPlainText(QStringLiteral("function helper(a, b)\nreturn true\nend\n"));
    move_to_end(editor);
    type(editor, QStringLiteral("hel"));
    REQUIRE(completion->is_open());
    REQUIRE(completion->selected_name().has_value());
    CHECK(*completion->selected_name() == QStringLiteral("helper"));

    press(editor, Qt::Key_Tab);
    CHECK(editor.toPlainText().endsWith(QStringLiteral("helper()")));
}

TEST_CASE("an argument hint follows the call the caret is inside", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    ScriptEditor   editor;
    ScriptCallTip* tip = editor.findChild<ScriptCallTip*>();
    REQUIRE(tip != nullptr);

    // `mem.read(0x1000, |` - the second parameter.
    editor.setPlainText(QStringLiteral("mem.read(0x1000, "));
    move_to_end(editor);
    REQUIRE(tip->is_visible());
    CHECK(tip->signature() == QStringLiteral("mem.read(address, token)"));
    CHECK(tip->active_parameter() == 1);

    // `mem.read(|` - the first parameter.
    QTextCursor at = editor.textCursor();
    at.setPosition(9);
    editor.setTextCursor(at);
    REQUIRE(tip->is_visible());
    CHECK(tip->signature() == QStringLiteral("mem.read(address, token)"));
    CHECK(tip->active_parameter() == 0);

    // Past the closing parenthesis there is no call to hint about.
    editor.setPlainText(QStringLiteral("mem.read(0x1000)"));
    move_to_end(editor);
    CHECK_FALSE(tip->is_visible());

    // A function the document defines shows its own parameter list.
    editor.setPlainText(QStringLiteral("local function helper(a, b)\nend\nhelper("));
    move_to_end(editor);
    REQUIRE(tip->is_visible());
    CHECK(tip->signature() == QStringLiteral("helper(a, b)"));

    // An unknown call shows nothing.
    editor.setPlainText(QStringLiteral("mystery("));
    move_to_end(editor);
    CHECK_FALSE(tip->is_visible());
}

TEST_CASE("an open completion list hides the argument hint", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    ScriptEditor           editor;
    ScriptCallTip*         tip        = editor.findChild<ScriptCallTip*>();
    ScriptCompletionPopup* completion = completion_of(editor);
    REQUIRE(tip != nullptr);
    REQUIRE(completion != nullptr);

    editor.setPlainText(QStringLiteral("mem.read("));
    move_to_end(editor);
    REQUIRE(tip->is_visible());

    press(editor, Qt::Key_Space, Qt::ControlModifier);
    REQUIRE(completion->is_open());
    CHECK_FALSE(tip->is_visible());
}
