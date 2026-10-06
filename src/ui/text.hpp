#pragma once

#include <string>
#include <string_view>

#include <QString>

namespace slopkit::ui
{
    // `std::string_view` -> QString for the UI's status text and model roles.
    // The length is passed explicitly, so an embedded NUL survives and a view
    // that is not NUL-terminated is never read past its end.
    [[nodiscard]] QString to_qstring(std::string_view text);

    // ASCII lowercase, the form the process list's search filters compare on.
    [[nodiscard]] std::string lowercase(std::string_view text);

} // namespace slopkit::ui
