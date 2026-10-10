#include "ui/components/script_highlighter.hpp"

#include <QSet>
#include <QTextCharFormat>
#include <QTextDocument>

#include "script/api_catalog.hpp"
#include "ui/theme.hpp"

namespace slopkit::ui::components
{

    namespace
    {
        // The block state a continuation line carries: 0 is ordinary source,
        // `kStringStateBase + 2*level` is inside a long string and
        // `kCommentStateBase + 2*level` is inside a long comment, where `level`
        // is the number of `=` signs in the opening bracket.
        constexpr int kNormalState      = 0;
        constexpr int kStringStateBase  = 1;
        constexpr int kCommentStateBase = 2;

        [[nodiscard]] int string_state(int level)
        {
            return kStringStateBase + 2 * level;
        }

        [[nodiscard]] int comment_state(int level)
        {
            return kCommentStateBase + 2 * level;
        }

        [[nodiscard]] bool is_comment_state(int state)
        {
            return state > 0 && state % 2 == 0;
        }

        [[nodiscard]] int level_of_state(int state)
        {
            return (state - (is_comment_state(state) ? kCommentStateBase : kStringStateBase)) / 2;
        }

        // The Lua keywords, in one place.
        const QSet<QString>& keywords()
        {
            static const QSet<QString> set {
                QStringLiteral("and"),      QStringLiteral("break"),  QStringLiteral("do"),    QStringLiteral("else"),
                QStringLiteral("elseif"),   QStringLiteral("end"),    QStringLiteral("false"), QStringLiteral("for"),
                QStringLiteral("function"), QStringLiteral("goto"),   QStringLiteral("if"),    QStringLiteral("in"),
                QStringLiteral("local"),    QStringLiteral("nil"),    QStringLiteral("not"),   QStringLiteral("or"),
                QStringLiteral("repeat"),   QStringLiteral("return"), QStringLiteral("then"),  QStringLiteral("true"),
                QStringLiteral("until"),    QStringLiteral("while"),
            };
            return set;
        }

        // Every global the script API and the Lua standard library define, straight
        // from the catalogue, so the highlighter cannot drift from the completion
        // list and a name is highlighted iff the engine actually exposes it.
        const QSet<QString>& api_globals()
        {
            static const QSet<QString> set = []
            {
                QSet<QString> names;
                for (const script::ApiEntry& entry : script::api_catalog())
                {
                    if (entry.name.find('.') == std::string_view::npos)
                    {
                        names.insert(QString::fromUtf8(entry.name.data(), static_cast<int>(entry.name.size())));
                    }
                }
                for (const std::string_view name : script::lua_standard_globals())
                {
                    names.insert(QString::fromUtf8(name.data(), static_cast<int>(name.size())));
                }
                return names;
            }();
            return set;
        }

        [[nodiscard]] QTextCharFormat coloured(const QColor& colour)
        {
            QTextCharFormat format;
            format.setForeground(colour);
            return format;
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

        // The offset just past the matching `]=*]` for `level`, or -1 when the
        // block ends before it closes.
        [[nodiscard]] int long_bracket_close(const QString& text, int from, int level)
        {
            const QString close = QStringLiteral("]") + QString(level, QLatin1Char('=')) + QStringLiteral("]");
            const int     at    = text.indexOf(close, from);
            return at < 0 ? -1 : at + close.size();
        }

        // The offset just past a short string starting at the quote `index`
        // handles escapes; an unterminated string ends with the block.
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

        [[nodiscard]] bool is_hex_digit(QChar ch)
        {
            return ch.isDigit() || (ch >= QLatin1Char('a') && ch <= QLatin1Char('f'))
                || (ch >= QLatin1Char('A') && ch <= QLatin1Char('F'));
        }

        // The offset just past `[pP]`/`[eE]` and its optional sign and digits.
        [[nodiscard]] int exponent_end(const QString& text, int index)
        {
            int i = index + 1;
            if (i < text.size() && (text.at(i) == QLatin1Char('+') || text.at(i) == QLatin1Char('-')))
            {
                ++i;
            }
            while (i < text.size() && text.at(i).isDigit())
            {
                ++i;
            }
            return i;
        }

        // The offset just past a number literal starting at `index` (which is a
        // digit, or a `.` followed by one): decimal, hex, float and exponent.
        [[nodiscard]] int number_end(const QString& text, int index)
        {
            int i = index;
            if (text.at(i) == QLatin1Char('0') && i + 1 < text.size()
                && (text.at(i + 1) == QLatin1Char('x') || text.at(i + 1) == QLatin1Char('X')))
            {
                i += 2;
                while (i < text.size() && is_hex_digit(text.at(i)))
                {
                    ++i;
                }
                if (i < text.size() && text.at(i) == QLatin1Char('.'))
                {
                    ++i;
                    while (i < text.size() && is_hex_digit(text.at(i)))
                    {
                        ++i;
                    }
                }
                if (i < text.size() && (text.at(i) == QLatin1Char('p') || text.at(i) == QLatin1Char('P')))
                {
                    i = exponent_end(text, i);
                }
                return i;
            }

            while (i < text.size() && text.at(i).isDigit())
            {
                ++i;
            }
            if (i < text.size() && text.at(i) == QLatin1Char('.'))
            {
                ++i;
                while (i < text.size() && text.at(i).isDigit())
                {
                    ++i;
                }
            }
            if (i < text.size() && (text.at(i) == QLatin1Char('e') || text.at(i) == QLatin1Char('E')))
            {
                i = exponent_end(text, i);
            }
            return i;
        }

        [[nodiscard]] bool is_identifier_start(QChar ch)
        {
            return ch.isLetter() || ch == QLatin1Char('_');
        }

        [[nodiscard]] bool is_identifier_part(QChar ch)
        {
            return ch.isLetterOrNumber() || ch == QLatin1Char('_');
        }
    } // namespace

    ScriptHighlighter::ScriptHighlighter(QTextDocument* document) : QSyntaxHighlighter(document) {}

    void ScriptHighlighter::highlightBlock(const QString& text)
    {
        const Theme theme = active_theme();

        const QTextCharFormat keyword_format = coloured(theme.syntax_keyword);
        const QTextCharFormat string_format  = coloured(theme.syntax_string);
        const QTextCharFormat comment_format = coloured(theme.syntax_comment);
        const QTextCharFormat number_format  = coloured(theme.syntax_number);
        const QTextCharFormat api_format     = coloured(theme.syntax_module);

        int index = 0;

        // A long string or long comment left open on the previous block carries
        // over; find where it closes, or colour the rest and keep the state.
        const int incoming = previousBlockState();
        if (incoming > 0)
        {
            const bool             comment = is_comment_state(incoming);
            const int              level   = level_of_state(incoming);
            const int              close   = long_bracket_close(text, 0, level);
            const QTextCharFormat& format  = comment ? comment_format : string_format;
            if (close < 0)
            {
                setFormat(0, text.size(), format);
                setCurrentBlockState(incoming);
                return;
            }
            setFormat(0, close, format);
            index = close;
        }

        while (index < text.size())
        {
            const QChar ch = text.at(index);

            if (ch.isSpace())
            {
                ++index;
                continue;
            }

            // `--` starts a comment; `--[[`/`--[=[` a long one.
            if (ch == QLatin1Char('-') && index + 1 < text.size() && text.at(index + 1) == QLatin1Char('-'))
            {
                int       level = 0;
                const int open  = long_bracket_open(text, index + 2, &level);
                if (open >= 0)
                {
                    const int close = long_bracket_close(text, open, level);
                    if (close < 0)
                    {
                        setFormat(index, text.size() - index, comment_format);
                        setCurrentBlockState(comment_state(level));
                        return;
                    }
                    setFormat(index, close - index, comment_format);
                    index = close;
                    continue;
                }
                setFormat(index, text.size() - index, comment_format);
                return; // a line comment runs to the end of the block
            }

            // `[[`/`[=[` starts a long string.
            if (ch == QLatin1Char('['))
            {
                int       level = 0;
                const int open  = long_bracket_open(text, index, &level);
                if (open >= 0)
                {
                    const int close = long_bracket_close(text, open, level);
                    if (close < 0)
                    {
                        setFormat(index, text.size() - index, string_format);
                        setCurrentBlockState(string_state(level));
                        return;
                    }
                    setFormat(index, close - index, string_format);
                    index = close;
                    continue;
                }
            }

            // A short string, single or double quoted.
            if (ch == QLatin1Char('"') || ch == QLatin1Char('\''))
            {
                const int end = short_string_end(text, index);
                setFormat(index, end - index, string_format);
                index = end;
                continue;
            }

            // A number, including a leading `.` fraction.
            if (ch.isDigit() || (ch == QLatin1Char('.') && index + 1 < text.size() && text.at(index + 1).isDigit()))
            {
                const int end = number_end(text, index);
                setFormat(index, end - index, number_format);
                index = end;
                continue;
            }

            // An identifier: a keyword, an API global, or nothing.
            if (is_identifier_start(ch))
            {
                int end = index + 1;
                while (end < text.size() && is_identifier_part(text.at(end)))
                {
                    ++end;
                }
                const QString word = text.mid(index, end - index);
                if (keywords().contains(word))
                {
                    setFormat(index, end - index, keyword_format);
                }
                else if (api_globals().contains(word))
                {
                    setFormat(index, end - index, api_format);
                }
                index = end;
                continue;
            }

            ++index;
        }

        setCurrentBlockState(kNormalState);
    }

} // namespace slopkit::ui::components
