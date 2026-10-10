#include "ui/components/script_context.hpp"

#include <QChar>
#include <QRegularExpression>
#include <QString>

namespace slopkit::ui::components::script_context
{
    namespace
    {
        [[nodiscard]] bool is_identifier_char(QChar character)
        {
            return character.isLetterOrNumber() || character == QLatin1Char('_');
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
        // bracket matching and hint suppression can ignore the ones inside them.
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

        [[nodiscard]] bool bracket_pairs(QChar character, QChar* open, QChar* close, bool* opening)
        {
            const std::pair<QChar, QChar> pairs[] = {
                {QLatin1Char('('), QLatin1Char(')')},
                {QLatin1Char('['), QLatin1Char(']')},
                {QLatin1Char('{'), QLatin1Char('}')}
            };
            for (const auto& [first, second] : pairs)
            {
                if (character == first)
                {
                    *opening = true;
                    *open    = first;
                    *close   = second;
                    return true;
                }
                if (character == second)
                {
                    *opening = false;
                    *open    = first;
                    *close   = second;
                    return true;
                }
            }
            return false;
        }
    } // namespace

    bool is_code_at(const QString& text, int position)
    {
        if (position < 0 || position > text.size())
        {
            return false;
        }
        if (text.isEmpty())
        {
            return true;
        }
        const std::vector<bool> mask  = code_mask(text);
        const int               index = position < text.size() ? position : text.size() - 1;
        return mask[static_cast<std::size_t>(index)];
    }

    std::optional<Identifier> identifier_at(const QString& text, int position)
    {
        if (position < 0 || position > text.size())
        {
            return std::nullopt;
        }

        const std::vector<bool> mask = code_mask(text);

        int begin = position;
        while (begin > 0 && is_identifier_char(text.at(begin - 1)) && mask[static_cast<std::size_t>(begin - 1)])
        {
            --begin;
        }
        int end = position;
        while (end < text.size() && is_identifier_char(text.at(end)) && mask[static_cast<std::size_t>(end)])
        {
            ++end;
        }

        Identifier identifier;
        identifier.begin = begin;
        identifier.end   = end;
        if (end > begin)
        {
            identifier.name = text.mid(begin, end - begin);
        }

        // A dot just before the word starts a dotted receiver ("mem.read").
        int receiver_end = begin;
        if (receiver_end > 0 && text.at(receiver_end - 1) == QLatin1Char('.'))
        {
            --receiver_end;
            int receiver_begin = receiver_end;
            while (receiver_begin > 0 && is_identifier_char(text.at(receiver_begin - 1))
                   && mask[static_cast<std::size_t>(receiver_begin - 1)])
            {
                --receiver_begin;
            }
            if (receiver_end > receiver_begin)
            {
                identifier.receiver = text.mid(receiver_begin, receiver_end - receiver_begin);
            }
        }

        if (identifier.name.isEmpty() && identifier.receiver.isEmpty())
        {
            return std::nullopt;
        }
        if (identifier.name.isEmpty() && position > 0 && !mask[static_cast<std::size_t>(position - 1)])
        {
            return std::nullopt; // the dot itself sits inside a comment or string
        }
        return identifier;
    }

    std::optional<Call> enclosing_call(const QString& text, int position)
    {
        if (position < 0 || position > text.size())
        {
            return std::nullopt;
        }

        const std::vector<bool> mask = code_mask(text);

        int open_paren = -1;
        int depth      = 0;
        for (int i = position - 1; i >= 0; --i)
        {
            if (!mask[static_cast<std::size_t>(i)])
            {
                continue;
            }
            const QChar character = text.at(i);
            if (character == QLatin1Char(')'))
            {
                ++depth;
            }
            else if (character == QLatin1Char('('))
            {
                if (depth == 0)
                {
                    open_paren = i;
                    break;
                }
                --depth;
            }
        }
        if (open_paren < 0)
        {
            return std::nullopt;
        }

        // The callee name, allowing whitespace between it and the parenthesis.
        int name_end = open_paren;
        while (name_end > 0 && text.at(name_end - 1).isSpace())
        {
            --name_end;
        }
        int name_begin = name_end;
        while (name_begin > 0 && is_identifier_char(text.at(name_begin - 1))
               && mask[static_cast<std::size_t>(name_begin - 1)])
        {
            --name_begin;
        }
        if (name_begin == name_end)
        {
            return std::nullopt;
        }

        Call call;
        call.name       = text.mid(name_begin, name_end - name_begin);
        call.open_paren = open_paren;

        int receiver_end = name_begin;
        if (receiver_end > 0 && text.at(receiver_end - 1) == QLatin1Char('.'))
        {
            --receiver_end;
            int receiver_begin = receiver_end;
            while (receiver_begin > 0 && is_identifier_char(text.at(receiver_begin - 1))
                   && mask[static_cast<std::size_t>(receiver_begin - 1)])
            {
                --receiver_begin;
            }
            call.receiver = text.mid(receiver_begin, receiver_end - receiver_begin);
        }

        int nesting = 0;
        for (int i = open_paren + 1; i < position; ++i)
        {
            if (!mask[static_cast<std::size_t>(i)])
            {
                continue;
            }
            const QChar character = text.at(i);
            if (character == QLatin1Char('(') || character == QLatin1Char('[') || character == QLatin1Char('{'))
            {
                ++nesting;
            }
            else if (character == QLatin1Char(')') || character == QLatin1Char(']') || character == QLatin1Char('}'))
            {
                --nesting;
            }
            else if (character == QLatin1Char(',') && nesting == 0)
            {
                ++call.active_parameter;
            }
        }

        return call;
    }

    std::vector<DocumentName> document_names(const QString& text)
    {
        // `local function f(...)` / `function f(...)` / `function a.b(...)` /
        // `local name`, in that precedence so `local function` is not read as a
        // variable named `function`.
        static const QRegularExpression pattern {QStringLiteral(
            R"((?:local\s+function\s+(\w+)\s*\(([^)]*)\))|(?:function\s+(\w+(?:[.:]\w+)*)\s*\(([^)]*)\))|(?:local\s+(\w+)))")};

        const std::vector<bool> mask = code_mask(text);

        std::vector<DocumentName>       names;
        QRegularExpressionMatchIterator matches = pattern.globalMatch(text);
        while (matches.hasNext())
        {
            const QRegularExpressionMatch match = matches.next();
            const int                     start = match.capturedStart();
            if (start < 0 || start >= static_cast<int>(mask.size()) || !mask[static_cast<std::size_t>(start)])
            {
                continue;
            }

            DocumentName name;
            if (match.capturedStart(1) >= 0)
            {
                name.name      = match.captured(1);
                name.signature = name.name + QLatin1Char('(') + match.captured(2) + QLatin1Char(')');
                name.function  = true;
            }
            else if (match.capturedStart(3) >= 0)
            {
                name.name      = match.captured(3);
                name.signature = name.name + QLatin1Char('(') + match.captured(4) + QLatin1Char(')');
                name.function  = true;
            }
            else if (match.capturedStart(5) >= 0)
            {
                name.name     = match.captured(5);
                name.function = false;
            }
            else
            {
                continue;
            }
            names.push_back(name);
        }
        return names;
    }

    int matching_bracket(const QString& text, int position)
    {
        if (position < 0 || position >= text.size())
        {
            return -1;
        }

        QChar open {0};
        QChar close {0};
        bool  opening = false;
        if (!bracket_pairs(text.at(position), &open, &close, &opening))
        {
            return -1;
        }

        const std::vector<bool> mask = code_mask(text);
        if (!mask[static_cast<std::size_t>(position)])
        {
            return -1;
        }

        if (opening)
        {
            int depth = 0;
            for (int i = position; i < text.size(); ++i)
            {
                if (!mask[static_cast<std::size_t>(i)])
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
            for (int i = position; i >= 0; --i)
            {
                if (!mask[static_cast<std::size_t>(i)])
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
} // namespace slopkit::ui::components::script_context
