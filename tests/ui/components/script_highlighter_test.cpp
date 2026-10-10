#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

#include "ui/components/script_highlighter.hpp"

#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>

namespace
{
    using slopkit::ui::components::ScriptHighlighter;

    // The foreground colour the highlighter gave the block text at `from` with
    // `length`, or an invalid colour when no format covers exactly that run.
    [[nodiscard]] QColor colour_at(const QTextBlock& block, int from, int length)
    {
        for (const QTextLayout::FormatRange& range : block.layout()->formats())
        {
            if (range.start == from && range.length == length)
            {
                return range.format.foreground().color();
            }
        }
        return QColor {};
    }
} // namespace

TEST_CASE("the Lua highlighter colours keywords, numbers, strings and comments", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    QTextDocument     document;
    ScriptHighlighter highlighter(&document);
    document.setPlainText(QStringLiteral("local x = 42 -- note"));
    highlighter.rehighlight();

    const QTextBlock          block = document.firstBlock();
    const slopkit::ui::Theme& theme = slopkit::ui::active_theme();

    CHECK(colour_at(block, 0, 5) == theme.syntax_keyword);  // local
    CHECK(colour_at(block, 6, 1) == QColor {});             // x stays plain
    CHECK(colour_at(block, 10, 2) == theme.syntax_number);  // 42
    CHECK(colour_at(block, 13, 7) == theme.syntax_comment); // -- note
}

TEST_CASE("the Lua highlighter colours the script API globals", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    QTextDocument     document;
    ScriptHighlighter highlighter(&document);
    document.setPlainText(QStringLiteral("mem.read(0x10)"));
    highlighter.rehighlight();

    const QTextBlock&         block = document.firstBlock();
    const slopkit::ui::Theme& theme = slopkit::ui::active_theme();

    CHECK(colour_at(block, 0, 3) == theme.syntax_module); // mem
    CHECK(colour_at(block, 9, 4) == theme.syntax_number); // 0x10
}

TEST_CASE("the Lua highlighter colours the API names it used to miss", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    QTextDocument     document;
    ScriptHighlighter highlighter(&document);
    document.setPlainText(QStringLiteral("hook.install(kHook)\naobscan(\"hp\", \"48 8B\")\nassemble(p, \"nop\")"));
    highlighter.rehighlight();

    const slopkit::ui::Theme& theme = slopkit::ui::active_theme();

    CHECK(colour_at(document.findBlockByNumber(0), 0, 4) == theme.syntax_module); // hook
    CHECK(colour_at(document.findBlockByNumber(1), 0, 7) == theme.syntax_module); // aobscan
    CHECK(colour_at(document.findBlockByNumber(2), 0, 8) == theme.syntax_module); // assemble
}

TEST_CASE("the Lua highlighter carries long constructs across blocks", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    const slopkit::ui::Theme& theme = slopkit::ui::active_theme();

    SECTION("a long comment spanning two blocks")
    {
        QTextDocument     document;
        ScriptHighlighter highlighter(&document);
        document.setPlainText(QStringLiteral("--[[ a\nb ]]\nlocal x"));
        highlighter.rehighlight();

        CHECK(colour_at(document.findBlockByNumber(0), 0, 6) == theme.syntax_comment);
        CHECK(colour_at(document.findBlockByNumber(1), 0, 4) == theme.syntax_comment);
        CHECK(colour_at(document.findBlockByNumber(2), 0, 5) == theme.syntax_keyword);
        CHECK(document.findBlockByNumber(2).userState() == 0); // no state left behind
    }

    SECTION("a long string spanning two blocks")
    {
        QTextDocument     document;
        ScriptHighlighter highlighter(&document);
        document.setPlainText(QStringLiteral("local s = [[ a\nb ]]\nprint(s)"));
        highlighter.rehighlight();

        CHECK(colour_at(document.findBlockByNumber(0), 0, 5) == theme.syntax_keyword);
        CHECK(colour_at(document.findBlockByNumber(0), 10, 4) == theme.syntax_string);
        CHECK(colour_at(document.findBlockByNumber(1), 0, 4) == theme.syntax_string);
        CHECK(colour_at(document.findBlockByNumber(2), 0, 5) == theme.syntax_module);
        CHECK(document.findBlockByNumber(2).userState() == 0);
    }

    SECTION("an unterminated long string does not hang")
    {
        QTextDocument     document;
        ScriptHighlighter highlighter(&document);
        document.setPlainText(QStringLiteral("local s = [[ never closed\nstill here"));
        highlighter.rehighlight();

        const QTextBlock last = document.findBlockByNumber(1);
        CHECK(colour_at(last, 0, last.text().size()) == theme.syntax_string);
    }
}

TEST_CASE("the Lua highlighter follows a theme switch", "[ui]")
{
    application();
    slopkit::ui::apply_theme(slopkit::ui::dark_theme());

    QTextDocument     document;
    ScriptHighlighter highlighter(&document);
    document.setPlainText(QStringLiteral("local x"));
    highlighter.rehighlight();

    CHECK(colour_at(document.firstBlock(), 0, 5) == slopkit::ui::dark_theme().syntax_keyword);

    slopkit::ui::apply_theme(slopkit::ui::light_theme());
    highlighter.rehighlight();

    CHECK(colour_at(document.firstBlock(), 0, 5) == slopkit::ui::light_theme().syntax_keyword);
    CHECK(slopkit::ui::dark_theme().syntax_keyword != slopkit::ui::light_theme().syntax_keyword);

    slopkit::ui::apply_theme(slopkit::ui::dark_theme());
}
