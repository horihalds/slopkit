#pragma once

#include <memory>

#include "script/structs_view.hpp"
#include "script/types.hpp"

struct lua_State;

namespace slopkit::script
{
    // What the `structs` binding reads through: the classes the run in progress
    // may name, and the engine's byte seam. The engine replaces `classes` before
    // each call, so a rename, a type change or a deletion in the editor is
    // visible on the next access.
    struct StructsContext
    {
        std::shared_ptr<const ClassIndex> classes;
        const MemoryApi*                  memory {};
    };

    // Registers the `structs` global on `state`; `context` must outlive it.
    void install_structs(lua_State* state, StructsContext& context);
}
