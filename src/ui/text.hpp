#pragma once

#include <string>
#include <string_view>

#include <QString>

namespace slopkit::ui
{
    [[nodiscard]] QString to_qstring(std::string_view text);

    [[nodiscard]] std::string lowercase(std::string_view text);

}
