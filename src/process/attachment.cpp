#include "process/attachment.hpp"

#include <format>

#include "core/log.hpp"
#include "core/log_categories.hpp"

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
        if (valid())
        {
            log::info(log::category::process, std::format("detached from {}", label()));
        }
        pid = 0;
        name.clear();
        exe_path.clear();
        plugin_id.clear();
        method       = AccessMethod::none;
        session_live = false;
    }

} // namespace slopkit::process
