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
    // The facts one generated hook script hands the runtime: where the window
    // sits, what the site held, the user's code and the instructions the hook
    // replaces. `pattern`/`module`/`offset` are for the engine's uniqueness
    // scan; `install` needs the rest to build the payload and patch the site,
    // and `remove` needs only `original`.
    struct Spec
    {
        std::string                site_name;
        std::string                cave_name;
        std::string                pattern;         // as parsed by scan::BytePattern
        std::string                module;          // empty = unrestricted scan
        std::size_t                offset {};       // the window's offset inside the match
        std::vector<std::byte>     original;        // the window's bytes; also the write-back
        std::string                code_text;       // the user's block, assembled at the cave
        std::string                trampoline_text; // the replaced instructions, operand-rewritten
        std::vector<std::int64_t>  address_args;    // site-relative deltas for the `%X` slots
        std::optional<std::size_t> cave_size;       // default: the exact payload size, rounded up
    };

    // Verifies `original` at `site`, maps a cave near it, builds the payload (the
    // user's code, the replaced instructions, then a jump back) and performs the
    // two target writes. Returns the cave address, or a reason and leaves the
    // target untouched; a cave mapped before the failure is freed again.
    [[nodiscard]] std::expected<std::uint64_t, std::string>
    install(const Spec& spec, const MemoryApi& memory, std::uint64_t site);

    // Writes `spec.original` back at `site` and frees `cave` (when it is not 0).
    [[nodiscard]] std::expected<void, std::string>
    remove(const Spec& spec, const MemoryApi& memory, std::uint64_t site, std::uint64_t cave);
} // namespace slopkit::script::hook
