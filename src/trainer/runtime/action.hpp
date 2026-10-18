#pragma once

#include <cstddef>
#include <string>

namespace slopkit::trainer
{
    // What the front-ends ask the session to do. `none` is a click on empty
    // space or an unrecognized line, which the caller simply drops.
    enum class ActionKind
    {
        none,
        toggle_freeze,
        set_value,
        activate_script,
        deactivate_script,
        refresh,
        quit,
    };

    struct Action
    {
        ActionKind  kind {ActionKind::none};
        std::size_t index {}; // 1-based inside the action's own row space
        std::string text;
    };

}
