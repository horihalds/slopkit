#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slopkit::script
{

    // `RunResult::output` is bounded so a script that prints in a loop cannot
    // flood the log: at most this many lines, each truncated to at most this
    // many bytes (with a trailing ellipsis when it was cut).
    inline constexpr std::size_t kMaxOutputLines = 1000;
    inline constexpr std::size_t kMaxOutputLine  = 4096;

    // The lifecycle hooks an address row's Active checkbox drives: ticking runs
    // `activate`, unticking runs `deactivate`.
    inline constexpr std::string_view kActivateHook   = "activate";
    inline constexpr std::string_view kDeactivateHook = "deactivate";

    // The outcome of one chunk: whether it ran to completion, the message of the
    // first error when it did not, the script's captured `print` lines in order
    // and the rendered scalar it returned (empty when the chunk returned
    // nothing or a table/function).
    struct RunResult
    {
        bool                     ok {false};
        std::string              error;
        std::vector<std::string> output;
        std::string              returned;
    };

    // The outcome of calling a script's `activate`/`deactivate` hook: `ok` is
    // the verdict the checkbox follows, `error` the chunk or hook failure text,
    // `message` the reason the hook itself returned when it refused, and
    // `output` the chunk's captured `print` lines.
    struct LifecycleResult
    {
        bool                     ok {false};
        std::string              error;
        std::string              message;
        std::vector<std::string> output;
    };

    // One mapped region of the target: where it starts, how big it is, whether
    // it can be read and which module owns it (empty for an anonymous mapping).
    struct MemoryRegion
    {
        std::uint64_t base {};
        std::uint64_t size {};
        bool          readable {};
        std::string   module;
    };

    // The memory seam a script may touch. The owner (the access worker) fills
    // these from its attached session; the engine never sees a `Session` or
    // `ProcessAccess`, so it is testable against an in-memory buffer.
    struct MemoryApi
    {
        std::function<std::size_t()>                                                                  pointer_size;
        std::function<std::expected<std::vector<std::byte>, std::string>(std::uint64_t, std::size_t)> read;
        std::function<std::expected<void, std::string>(std::uint64_t, std::span<const std::byte>)>    write;
        // Every mapped region the target exposes. Absent when the seam has no
        // region metadata at all (then `aobscan` reports "no target is
        // attached").
        std::function<std::expected<std::vector<MemoryRegion>, std::string>()>                        regions;
    };

    // The symbol seam a script's `rsymbol`/`ssymbol`/`usymbol` write through. The
    // owner (the access worker) fills these from the shared `SymbolTable`; the
    // engine never sees the table itself, so it is testable on its own.
    struct SymbolApi
    {
        // Registers `name` or overwrites its value; a non-empty error text
        // becomes a Lua error.
        std::function<std::expected<void, std::string>(std::string_view name, std::uint64_t value)> set;
        // Removes `name`; an unknown name is a no-op.
        std::function<std::expected<void, std::string>(std::string_view name)>                      remove;
        // The value `name` currently has, matched case-insensitively; empty when
        // the shared table does not know it. Read-only: the engine never
        // publishes through this closure, it only consults it for labels-first
        // resolution.
        std::function<std::optional<std::uint64_t>(std::string_view name)>                          lookup;
    };

    // Guards a runaway chunk: at most this many VM instructions, and at most
    // this many wall-clock seconds, before the run is aborted as an error.
    struct EngineConfig
    {
        std::size_t instruction_budget {20'000'000};
        double      timeout_seconds {5.0};
    };

} // namespace slopkit::script
