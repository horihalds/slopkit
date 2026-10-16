#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "trainer/payload.hpp"
#include "trainer/runtime/resolve.hpp"

namespace slopkit::trainer::console
{
    enum class CommandKind
    {
        none,
        freeze,
        set_value,
        refresh,
        list,
        quit,
        help,
        invalid,
    };

    struct Command
    {
        CommandKind kind {CommandKind::none};
        std::size_t index {}; // 1-based, as displayed
        std::string value;    // the text after `s <N>`
        std::string text;     // the offending input for `invalid`
    };

    // `f<N>` / `f <N>` freeze, `s <N> <value>` set, `r` re-resolve, `l` re-list,
    // `q` quit and `?` help; anything else is `invalid`.
    [[nodiscard]] Command parse_command(std::string_view line);

    [[nodiscard]] std::string render_header(const Payload& payload, std::uint32_t pid);
    [[nodiscard]] std::string render_rows(const Payload& payload, std::span<const LiveEntry> live);
    [[nodiscard]] std::string render_help();
    [[nodiscard]] std::string render_usage();

}
