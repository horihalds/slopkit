#include "expr/expression.hpp"

#include <charconv>
#include <format>

namespace slopkit::expr
{

    namespace
    {
        std::string_view trim(std::string_view value)
        {
            const auto first = value.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos)
            {
                return {};
            }
            const auto last = value.find_last_not_of(" \t\r\n");
            return value.substr(first, last - first + 1);
        }

        void skip_ws(std::string_view text, std::size_t& pos)
        {
            while (pos < text.size()
                   && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' || text[pos] == '\n'))
            {
                ++pos;
            }
        }

        // Reads a token up to the next '+' or ']' and trims it.
        std::string_view read_token(std::string_view text, std::size_t& pos)
        {
            const std::size_t start = pos;
            while (pos < text.size() && text[pos] != '+' && text[pos] != ']')
            {
                ++pos;
            }
            return trim(text.substr(start, pos - start));
        }

        constexpr std::size_t kMaxBracketDepth {32};

        // Parses `operand ( "+" offset )*` at `pos`, appending to `expression`.
        // A '[' operand recurses into its chain and closes with one dereference.
        std::expected<void, Error> parse_chain(
            std::string_view text, std::size_t& pos, Expression& expression, std::size_t depth, bool& saw_bracket)
        {
            skip_ws(text, pos);
            if (pos >= text.size())
            {
                return std::unexpected(Error {"the expression is empty"});
            }

            if (text[pos] == '[')
            {
                if (depth >= kMaxBracketDepth)
                {
                    return std::unexpected(Error {"more than 32 pointer levels"});
                }
                saw_bracket = true;
                ++pos; // skip '['
                skip_ws(text, pos);
                if (pos >= text.size())
                {
                    return std::unexpected(Error {"unbalanced '['"});
                }
                if (text[pos] == ']')
                {
                    return std::unexpected(Error {"an empty '[]' group"});
                }
                if (text[pos] == '+')
                {
                    return std::unexpected(Error {"missing offset after '+'"});
                }

                const std::size_t offsets_before = expression.offsets.size();
                const auto        group          = parse_chain(text, pos, expression, depth + 1, saw_bracket);
                if (!group)
                {
                    return group;
                }
                skip_ws(text, pos);
                if (pos >= text.size() || text[pos] != ']')
                {
                    return std::unexpected(Error {"unbalanced '['"});
                }
                ++pos; // skip ']'

                // A group whose chain carried no offsets still dereferences its
                // base, so it owns an entry.
                if (expression.offsets.size() == offsets_before)
                {
                    expression.offsets.push_back(Offset {});
                }
                ++expression.offsets.back().dereferences;
            }
            else if (text[pos] == ']')
            {
                return std::unexpected(Error {"unbalanced ']'"});
            }
            else
            {
                const auto token = read_token(text, pos);
                if (token.empty())
                {
                    return std::unexpected(Error {"the expression is empty"});
                }
                if (expression.base.empty())
                {
                    expression.base = std::string(token);
                }
            }

            while (true)
            {
                skip_ws(text, pos);
                if (pos >= text.size() || text[pos] != '+')
                {
                    break;
                }
                ++pos; // skip '+'
                skip_ws(text, pos);
                const auto token = read_token(text, pos);
                if (token.empty())
                {
                    return std::unexpected(Error {"missing offset after '+'"});
                }
                const auto value = parse_literal(token);
                if (!value)
                {
                    return std::unexpected(Error {std::format("invalid offset '{}'", token)});
                }
                expression.offsets.push_back(Offset {*value, 0});
            }
            return {};
        }
    } // namespace

    std::size_t Expression::pointer_levels() const noexcept
    {
        std::size_t levels = 0;
        for (const auto& offset : offsets)
        {
            levels += offset.dereferences;
        }
        return levels;
    }

    std::expected<std::uint64_t, Error> parse_literal(std::string_view text)
    {
        text = trim(text);
        if (text.empty())
        {
            return std::unexpected(Error {"the literal is empty"});
        }

        std::string_view digits = text;
        bool             hex    = true;
        if (digits.size() >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
        {
            digits.remove_prefix(2);
            hex = true;
        }
        else if (digits.front() == '#')
        {
            digits.remove_prefix(1);
            hex = false;
        }
        if (digits.empty())
        {
            return std::unexpected(Error {std::format("invalid literal '{}'", text)});
        }

        std::uint64_t value {};
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value, hex ? 16 : 10);
        if (error != std::errc {} || end != digits.data() + digits.size())
        {
            return std::unexpected(Error {std::format("invalid literal '{}'", text)});
        }
        return value;
    }

    std::expected<Expression, Error> parse(std::string_view text)
    {
        text = trim(text);
        if (text.empty())
        {
            return std::unexpected(Error {"the expression is empty"});
        }

        Expression  expression;
        std::size_t pos         = 0;
        bool        saw_bracket = false;
        const auto  result      = parse_chain(text, pos, expression, 0, saw_bracket);
        if (!result)
        {
            return std::unexpected(result.error());
        }
        if (pos != text.size())
        {
            if (text[pos] == ']')
            {
                return std::unexpected(Error {"unbalanced ']'"});
            }
            const auto token = read_token(text, pos);
            return std::unexpected(Error {std::format("invalid offset '{}'", token)});
        }
        if (expression.base.empty())
        {
            return std::unexpected(Error {"the expression is empty"});
        }

        // A bracket-free expression keeps the legacy rule: one dereference
        // between consecutive offsets, and none after the last.
        if (!saw_bracket)
        {
            for (std::size_t i = 0; i + 1 < expression.offsets.size(); ++i)
            {
                expression.offsets[i].dereferences = 1;
            }
        }
        return expression;
    }

} // namespace slopkit::expr
