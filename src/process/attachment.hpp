#pragma once

#include <string>

#include "process/access.hpp"
#include "process/types.hpp"

namespace slopkit::process
{

    // The single attachment shared by the whole application. The top bar, the
    // address list and the memory viewer all read the same session instead of
    // each holding their own.
    struct AttachedTarget
    {
        ProcessId   pid {};
        std::string name;
        std::string plugin_id;
        Session     session;

        // True once a session is live and a pid is known.
        [[nodiscard]] bool valid() const noexcept
        {
            return session && pid != 0;
        }

        // "<name> (<pid>) - <plugin>", or "No Process Selected" when detached.
        [[nodiscard]] std::string label() const;

        void clear();
    };

} // namespace slopkit::process
