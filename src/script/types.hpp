#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
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

    // The memory seam a script may touch. The owner (the access worker) fills
    // these from its attached session; the engine never sees a `Session` or
    // `ProcessAccess`, so it is testable against an in-memory buffer.
    struct MemoryApi
    {
        std::function<std::size_t()>                                                                  pointer_size;
        std::function<std::expected<std::vector<std::byte>, std::string>(std::uint64_t, std::size_t)> read;
        std::function<std::expected<void, std::string>(std::uint64_t, std::span<const std::byte>)>    write;
    };

    // Guards a runaway chunk: at most this many VM instructions, and at most
    // this many wall-clock seconds, before the run is aborted as an error.
    struct EngineConfig
    {
        std::size_t instruction_budget {20'000'000};
        double      timeout_seconds {5.0};
    };

} // namespace slopkit::script
