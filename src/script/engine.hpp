#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "script/structs_view.hpp"
#include "script/types.hpp"

namespace slopkit::script
{
    struct UpdateResult
    {
        bool                         ran {};
        bool                         ok {true};
        std::string                  error;
        std::string                  message;
        std::vector<std::string>     output;
        std::optional<std::uint64_t> interval_ms;
    };

    class Engine
    {
    public:
        explicit Engine(MemoryApi api, SymbolApi symbols = {}, EngineConfig config = {});
        ~Engine();

        Engine(const Engine&)            = delete;
        Engine& operator=(const Engine&) = delete;
        Engine(Engine&&) noexcept;
        Engine& operator=(Engine&&) noexcept;

        [[nodiscard]] RunResult run(std::string_view chunk);

        [[nodiscard]] LifecycleResult run_lifecycle(std::string_view chunk, std::string_view function);

        // The classes the `structs` global of the next call may name, as the
        // snapshot the caller took: the engine never reads the editing model.
        void set_classes(std::shared_ptr<const ClassIndex> classes);

        [[nodiscard]] UpdateResult run_update(std::string_view chunk);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    [[nodiscard]] std::expected<void, std::string> check_syntax(std::string_view chunk);

}
