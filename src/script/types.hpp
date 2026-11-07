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

#include "expr/resolver.hpp"

namespace slopkit::script
{
    inline constexpr std::size_t kMaxOutputLines = 1000;
    inline constexpr std::size_t kMaxOutputLine  = 4096;

    inline constexpr std::string_view kActivateHook   = "activate";
    inline constexpr std::string_view kDeactivateHook = "deactivate";
    inline constexpr std::string_view kUpdateHook     = "update";

    struct RunResult
    {
        bool                     ok {false};
        std::string              error;
        std::vector<std::string> output;
        std::string              returned;
    };

    struct LifecycleResult
    {
        bool                         ok {false};
        std::string                  error;
        std::string                  message;
        std::vector<std::string>     output;
        std::optional<std::uint64_t> interval_ms;
    };

    struct MemoryRegion
    {
        std::uint64_t base {};
        std::uint64_t size {};
        bool          readable {};
        std::string   module;
    };

    struct MemoryApi
    {
        std::function<std::size_t()>                                                                   pointer_size;
        std::function<std::expected<std::vector<std::byte>, std::string>(std::uint64_t, std::size_t)>  read;
        std::function<std::expected<void, std::string>(std::uint64_t, std::span<const std::byte>)>     write;
        std::function<std::expected<std::vector<MemoryRegion>, std::string>()>                         regions;
        std::function<std::expected<std::uint64_t, std::string>(std::size_t size, std::uint64_t near)> allocate;
        // A hook's cave is code and a script's own `alloc` is a slot the target
        // writes, so a target that can tell the two apart provides this; one that
        // maps every allocation writably and executably need not.
        std::function<std::expected<std::uint64_t, std::string>(std::size_t size, std::uint64_t near)>
                                                                                                 allocate_executable;
        std::function<std::expected<void, std::string>(std::uint64_t address)>                   deallocate;
        std::function<std::expected<bool, std::string>(std::uint64_t address, std::size_t size)> validate;
        std::function<std::expected<std::vector<expr::ModuleRef>, std::string>()>                modules;
    };

    struct SymbolApi
    {
        std::function<std::expected<void, std::string>(std::string_view name, std::uint64_t value)> set;
        std::function<std::expected<void, std::string>(std::string_view name)>                      remove;
        std::function<std::optional<std::uint64_t>(std::string_view name)>                          lookup;
    };

    struct EngineConfig
    {
        std::size_t instruction_budget {20'000'000};
        double      timeout_seconds {5.0};
    };

}
