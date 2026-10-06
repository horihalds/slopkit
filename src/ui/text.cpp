#include "ui/text.hpp"

#include <cctype>
#include <ranges>

namespace slopkit::ui
{
    QString to_qstring(std::string_view text)
    {
        return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    }

    std::string lowercase(std::string_view text)
    {
        std::string result(text);
        std::ranges::transform(result,
                               result.begin(),
                               [](char character)
                               {
                                   return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
                               });
        return result;
    }

} // namespace slopkit::ui
