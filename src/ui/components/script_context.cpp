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

        [[nodiscard]] int long_bracket_close(const QString& text, int from, int level)
        {
            const QString close = QStringLiteral("]") + QString(level, QLatin1Char('=')) + QStringLiteral("]");
            const int     at    = text.indexOf(close, from);
            return at < 0 ? -1 : at + close.size();
        }

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

        // How many `variable = other` hops the binding walk may follow, so a
        // cycle of aliases stops instead of recursing forever.
        constexpr int kMaxBindingDepth = 8;

        [[nodiscard]] bool is_plain_identifier(const QString& text)
        {
            if (text.isEmpty() || (!text.at(0).isLetter() && text.at(0) != QLatin1Char('_')))
            {
                return false;
            }
            for (const QChar character : text)
            {
                if (!is_identifier_char(character))
                {
                    return false;
                }
            }
            return true;
        }

        struct Assignment
        {
            QString source;   // the right-hand side of the `=`
            int     begin {}; // where that side starts in the text
            int     end {};   // where it ends
        };

        // Every assignment to `variable` that sits in code, in the order the
        // text writes them; a statement ends at the line's break or its `;`.
        [[nodiscard]] std::vector<Assignment> assignments_of(const QString& text, const QString& variable)
        {
            const QRegularExpression expression {
                QStringLiteral(R"((?:local\s+)?\b%1\b\s*=\s*([^\n;]*))").arg(QRegularExpression::escape(variable))};

            const std::vector<bool>         mask = code_mask(text);
            std::vector<Assignment>         found;
            QRegularExpressionMatchIterator matches = expression.globalMatch(text);
            while (matches.hasNext())
            {
                const QRegularExpressionMatch match = matches.next();
                const int                     start = match.capturedStart();
                if (start < 0 || start >= static_cast<int>(mask.size()) || !mask[static_cast<std::size_t>(start)])
                {
                    continue;
                }
                found.push_back({match.captured(1),
                                 static_cast<int>(match.capturedStart(1)),
                                 static_cast<int>(match.capturedEnd(1))});
            }
            return found;
        }

        // The assignment that decides the variable's value at `position`: the
        // last one the text writes at or before it.
        [[nodiscard]] const Assignment* deciding_assignment(const std::vector<Assignment>& found, int position)
        {
            const Assignment* deciding = nullptr;
            for (const Assignment& assignment : found)
            {
                if (assignment.end <= position)
                {
                    deciding = &assignment;
                }
            }
            return deciding;
        }

        // The class name a `structs.get("...")` call at the start of `source`
        // names, with the text after its closing parenthesis in `rest`.
        [[nodiscard]] std::optional<QString> structs_get_call(const QString& source, QString* rest)
        {
            static const QRegularExpression pattern {
                QStringLiteral(R"(^\s*structs\s*\.\s*get\s*\(\s*(['"])(.*?)\1\s*\)(.*)$)")};
            const QRegularExpressionMatch match = pattern.match(source);
            if (!match.hasMatch())
            {
                return std::nullopt;
            }
            if (rest != nullptr)
            {
                *rest = match.captured(3);
            }
            return match.captured(2);
        }

        [[nodiscard]] std::optional<QString>
        instance_class_at(const QString& text, const QString& variable, int position, int depth);

        // The class a variable holding a class handle names at `position`,
        // following one handle variable assigned from another.
        [[nodiscard]] std::optional<QString>
        handle_class_at(const QString& text, const QString& variable, int position, int depth)
        {
            if (depth > kMaxBindingDepth)
            {
                return std::nullopt;
            }

            static const QRegularExpression alias {QStringLiteral(R"(^\s*(\w+)\s*$)")};

            const std::vector<Assignment> found    = assignments_of(text, variable);
            const Assignment*             deciding = deciding_assignment(found, position);
            if (deciding == nullptr)
            {
                return std::nullopt;
            }
            if (const std::optional<QString> name = structs_get_call(deciding->source, nullptr))
            {
                return name;
            }

            const QRegularExpressionMatch match = alias.match(deciding->source);
            if (match.hasMatch())
            {
                return handle_class_at(text, match.captured(1), deciding->end, depth + 1);
            }
            return std::nullopt;
        }

        // The class one assignment's right-hand side names, when it names an
        // instance at all; the nested lookups it triggers see the bindings in
        // force where the right-hand side sits.
        [[nodiscard]] std::optional<QString>
        class_from_source(const QString& text, const Assignment& assignment, int depth)
        {
            if (depth > kMaxBindingDepth)
            {
                return std::nullopt;
            }

            QString rest;
            if (const std::optional<QString> name = structs_get_call(assignment.source, &rest))
            {
                static const QRegularExpression at_call {QStringLiteral(R"(^\s*:\s*at\s*\()")};
                return at_call.match(rest).hasMatch() ? name : std::nullopt;
            }

            static const QRegularExpression method {QStringLiteral(R"(^\s*(\w+)\s*:\s*at\s*\()")};
            const QRegularExpressionMatch   called = method.match(assignment.source);
            if (called.hasMatch())
            {
                return handle_class_at(text, called.captured(1), assignment.end, depth + 1);
            }

            static const QRegularExpression plain {QStringLiteral(R"(^\s*(\w+)\s*$)")};
            const QRegularExpressionMatch   named = plain.match(assignment.source);
            if (named.hasMatch())
            {
                return instance_class_at(text, named.captured(1), assignment.end, depth + 1);
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<QString>
        instance_class_at(const QString& text, const QString& variable, int position, int depth)
        {
            if (depth > kMaxBindingDepth)
            {
                return std::nullopt;
            }
            const std::vector<Assignment> found    = assignments_of(text, variable);
            const Assignment*             deciding = deciding_assignment(found, position);
            return deciding == nullptr ? std::nullopt : class_from_source(text, *deciding, depth);
        }
    }

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
            return std::nullopt;
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

    std::optional<QString> instance_class(const QString& text, int position, const QString& variable)
    {
        if (!is_plain_identifier(variable) || position < 0 || position > text.size())
        {
            return std::nullopt;
        }
        return instance_class_at(text, variable, position, 0);
    }
}
