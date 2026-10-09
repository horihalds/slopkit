#pragma once

#include <memory>
#include <string_view>

#include "script/types.hpp"

namespace slopkit::script
{

    // One Lua state with the `mem` table bound over a `MemoryApi` and the
    // `rsymbol`/`ssymbol`/`usymbol` globals bound over a `SymbolApi`. `run()` is
    // not re-entrant; callers serialize runs. The state (and its globals)
    // survives between runs, so a script can remember helper functions across
    // calls.
    //
    // sol2 and the Lua headers stay inside `engine.cpp`: this header only needs
    // the plain data types above.
    class Engine
    {
    public:
        explicit Engine(MemoryApi api, SymbolApi symbols = {}, EngineConfig config = {});
        ~Engine();

        Engine(const Engine&)            = delete;
        Engine& operator=(const Engine&) = delete;
        Engine(Engine&&) noexcept;
        Engine& operator=(Engine&&) noexcept;

        // Runs one chunk. Syntax, runtime and `mem` errors come back as
        // `RunResult::error`; nothing is thrown and the state stays usable.
        [[nodiscard]] RunResult run(std::string_view chunk);

        // Runs `chunk` and then its named global function (`kActivateHook` /
        // `kDeactivateHook`). A chunk error never reaches the hook; a missing
        // or non-function global is reported as `error`; a hook returning
        // nothing or a `true` first value succeeds, while `false` fails with an
        // optional second return value as `message`. Nothing is thrown.
        [[nodiscard]] LifecycleResult run_lifecycle(std::string_view chunk, std::string_view function);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace slopkit::script
