#pragma once

#include <string>

#include "process/types.hpp"

namespace slopkit::process
{
    struct AttachedTarget
    {
        ProcessId    pid {};
        std::string  name;
        std::string  exe_path;
        std::string  plugin_id;
        AccessMethod method {AccessMethod::none};
        bool         session_live {false};

        [[nodiscard]] bool valid() const noexcept
        {
            return session_live && pid != 0;
        }

        [[nodiscard]] std::string label() const;

        void clear();
    };

}
