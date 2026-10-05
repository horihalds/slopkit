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
    } // namespace

    std::size_t Expression::pointer_levels() const noexcept
    {
        return offsets.empty() ? 0 : offsets.size() - 1;
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

        Expression expression;

        const auto first_plus = text.find('+');
        expression.base = std::string(trim(first_plus == std::string_view::npos ? text : text.substr(0, first_plus)));
        if (expression.base.empty())
        {
            return std::unexpected(Error {"the expression is empty"});
        }

        std::size_t start = first_plus;
        while (start != std::string_view::npos && start < text.size())
        {
            ++start; // skip the '+'
            const auto next = text.find('+', start);
            const auto token =
                trim(text.substr(start, next == std::string_view::npos ? std::string_view::npos : next - start));
            if (token.empty())
            {
                return std::unexpected(Error {"missing offset after '+'"});
            }

            const auto value = parse_literal(token);
            if (!value)
            {
                return std::unexpected(Error {std::format("invalid offset '{}'", token)});
            }
            expression.offsets.push_back(Offset {*value});
            start = next;
        }
        return expression;
    }

} // namespace slopkit::expr
