#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "script/types.hpp"

namespace slopkit::script::hook
{
    struct Spec
    {
        std::string                site_name;
        std::string                cave_name;
        std::string                pattern;
        std::string                module;
        std::size_t                offset {};
        std::vector<std::byte>     original;
        std::string                code_text;
        std::string                trampoline_text;
        std::vector<std::int64_t>  address_args;
        std::optional<std::size_t> cave_size;
    };

    [[nodiscard]] std::expected<std::uint64_t, std::string>
    install(const Spec& spec, const MemoryApi& memory, std::uint64_t site);

    [[nodiscard]] std::expected<void, std::string>
    remove(const Spec& spec, const MemoryApi& memory, std::uint64_t site, std::uint64_t cave);
}
