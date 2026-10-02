#include "process/attachment.hpp"

#include <format>

namespace slopkit::process
{

    std::string AttachedTarget::label() const
    {
        if (!valid())
        {
            return "No Process Selected";
        }
        const std::string_view display = name.empty() ? std::string_view {"process"} : std::string_view {name};
        if (plugin_id.empty())
        {
            return std::format("{} ({})", display, pid);
        }
        return std::format("{} ({}) - {}", display, pid, plugin_id);
    }

    void AttachedTarget::clear()
    {
        pid = 0;
        name.clear();
        plugin_id.clear();
        method       = AccessMethod::none;
        session_live = false;
    }

} // namespace slopkit::process
