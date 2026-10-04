#include "table/table_settings.hpp"

namespace slopkit::table
{

    bool TableSettings::empty() const noexcept
    {
        return target_process.empty() && !auto_attach && !match_exe_path;
    }

} // namespace slopkit::table
