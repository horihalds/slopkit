#pragma once

#include <string>

#include "process/types.hpp"

namespace slopkit::process
{

    // Metadata of the single app-wide attachment. The live Session is owned by
    // the AccessWorker; the UI only keeps the identity of the attached target,
    // so no target access can happen on the UI thread.
    struct AttachedTarget
    {
        ProcessId    pid {};
        std::string  name;
        std::string  plugin_id;
        AccessMethod method {AccessMethod::none};
        bool         session_live {false};

        // True once a session is live and a pid is known.
        [[nodiscard]] bool valid() const noexcept
        {
            return session_live && pid != 0;
        }

        // "<name> (<pid>) - <plugin>", or "No Process Selected" when detached.
        [[nodiscard]] std::string label() const;

        void clear();
    };

} // namespace slopkit::process
