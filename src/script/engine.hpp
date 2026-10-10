#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "script/types.hpp"

namespace slopkit::script
{

    // The outcome of one `update` tick: `ran` says the chunk defined and called
    // a hook of its own, `ok` whether the tick succeeded (the chunk ran and the
    // hook, when there was one, accepted), `error` the chunk or hook failure
    // text, `message` the reason the hook returned when it refused, and `output`
    // the tick's captured `print` lines.
    struct UpdateResult
    {
        bool                     ran {};    // the chunk defined an `update` of its own
        bool                     ok {true}; // a `false` verdict means the tick failed
        std::string              error;     // a chunk or hook error
        std::string              message;   // the hook's own reason for a refusal
        std::vector<std::string> output;    // the lines the tick printed (not logged)
    };

    // One Lua state with the `mem` table bound over a `MemoryApi`, the
    // `rsymbol`/`ssymbol`/`usymbol` globals bound over a `SymbolApi` and the
    // script-local `rlabel`/`slabel`/`ulabel` registry the engine owns. A name
    // resolved by a script function is looked up in the labels first, then in
    // the shared symbols. `run()` is not re-entrant; callers serialize runs. The
    // state (and its globals) survives between runs, so a script can remember
    // helper functions across calls, while the labels are dropped when a
    // different chunk runs.
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

        // Runs `chunk` and then calls its own `kUpdateHook` global, if the chunk
        // defined one. Unlike the lifecycle hooks a missing `update` is legal
        // and silent, so only a function this very chunk defines is called (a
        // global left behind by another script never is). A chunk error never
        // reaches the hook; the verdict maps exactly like `run_lifecycle`. The
        // tick's captured `print` lines come back in `output` for the tests and
        // are not meant for the Log window.
        [[nodiscard]] UpdateResult run_update(std::string_view chunk);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    // Compiles `chunk` (chunk name `@script`, exactly as a run uses) and runs
    // nothing: an empty result means valid Lua, otherwise the compiler's own
    // message ("script:3: 'end' expected ..."). It uses its own throwaway Lua
    // state that binds no library and no `mem` table, so no target is touched
    // and no session is needed.
    [[nodiscard]] std::expected<void, std::string> check_syntax(std::string_view chunk);

} // namespace slopkit::script
