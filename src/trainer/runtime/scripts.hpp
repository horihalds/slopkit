#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "expr/resolver.hpp"
#include "reclass/project.hpp"
#include "script/engine.hpp"
#include "script/symbols.hpp"
#include "trainer/payload.hpp"
#include "trainer/runtime/caves.hpp"
#include "trainer/runtime/target.hpp"

namespace slopkit::trainer
{
    // One payload script as the console shows it and the loop ticks it. `output`
    // and `message` are the last hook's, so a failed script keeps the diagnosis
    // that explains its state.
    struct ScriptState
    {
        bool                                  active {false};
        bool                                  failed {false};
        std::string                           message;
        std::vector<std::string>              output;
        std::uint64_t                         interval_ms {0};
        std::chrono::steady_clock::time_point next_update {};
    };

    // Runs the payload's scripts against the target through the application's own
    // script engine, so a table's scripts behave in the trainer exactly as they do
    // in slopkit. Scripts start idle: nothing runs until the user activates one,
    // and the symbols a script publishes feed expression resolution through the
    // resolver's symbol snapshot.
    class ScriptHost
    {
    public:
        ScriptHost(const Target& target, const Payload& payload, std::uint64_t fallback_interval_ms);

        [[nodiscard]] std::size_t                       size() const noexcept;
        [[nodiscard]] std::span<const ScriptState>      states() const noexcept;
        [[nodiscard]] const std::vector<PayloadScript>& scripts() const noexcept;

        // The symbols the active scripts published so far, for the resolver.
        [[nodiscard]] std::vector<expr::SymbolRef> symbols() const;

        // The reason the payload's class project could not be read; empty when
        // `structs` works. Reported once by the caller.
        [[nodiscard]] const std::string& class_error() const noexcept;

        // `index` is the payload script's position, 0-based. Activating runs the
        // `activate` hook, deactivating the `deactivate` hook; a failed hook marks
        // the script failed and leaves every other script alone.
        [[nodiscard]] std::expected<void, std::string> activate(std::size_t index);
        [[nodiscard]] std::expected<void, std::string> deactivate(std::size_t index);

        // Points the host at a new run of the target, dropping the old engine's
        // state. Scripts are left inactive; the caller decides which to activate.
        void rebind(const Target& target);

        // Marks every script inactive without running its `deactivate` hook. Used
        // when the target is already gone, so a hook could not succeed anyway.
        void disable_all() noexcept;

        // Runs the `update` hook of every active script whose interval has elapsed.
        void tick(std::chrono::steady_clock::time_point now);

        // The hook output and message of one script, as the console prints them.
        [[nodiscard]] std::string output_text(std::size_t index) const;

    private:
        [[nodiscard]] script::MemoryApi memory_api();

        // Records a hook outcome: what it printed, the interval it asked for, and
        // whether it failed.
        void note(std::size_t index, const std::vector<std::string>& output, const std::string& message, bool failed);
        void adopt_interval(std::size_t                           index,
                            const std::optional<std::uint64_t>&   interval,
                            std::chrono::steady_clock::time_point now);

        const Target*                   target_;
        std::vector<PayloadScript>      scripts_;
        std::uint64_t                   fallback_interval_ms_ {250};
        script::SymbolTable             symbols_;
        CaveAllocator                   caves_;
        std::unique_ptr<script::Engine> engine_;
        std::vector<ScriptState>        states_;
        reclass::Project                project_;
        std::string                     class_error_;
    };

}
