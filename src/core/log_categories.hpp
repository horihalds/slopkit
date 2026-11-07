#pragma once

#include <string_view>

namespace slopkit::log::category
{
    inline constexpr std::string_view app {"app"};
    inline constexpr std::string_view plugin {"plugin"};
    inline constexpr std::string_view process {"process"};
    inline constexpr std::string_view memory {"memory"};
    inline constexpr std::string_view scan {"scan"};
    inline constexpr std::string_view table {"table"};
    inline constexpr std::string_view script {"script"};
    inline constexpr std::string_view debug {"debug"};
    inline constexpr std::string_view ui {"ui"};
}
