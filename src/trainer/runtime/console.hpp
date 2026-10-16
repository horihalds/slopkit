#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "trainer/payload.hpp"
#include "trainer/runtime/resolve.hpp"
#include "trainer/runtime/scripts.hpp"

namespace slopkit::trainer::console
{
    enum class CommandKind
    {
        none,
        freeze,
        set_value,
        activate_script,
        deactivate_script,
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

    // `f<N>` / `f <N>` freeze, `s <N> <value>` set, `a<N>` / `a <N>` activate a
    // script, `d<N>` / `d <N>` deactivate one, `r` re-resolve, `l` re-list,
    // `q` quit and `?` help; anything else is `invalid`. Value entries and scripts
    // are numbered independently.
    [[nodiscard]] Command parse_command(std::string_view line);

    [[nodiscard]] std::string render_header(const Payload& payload, std::uint32_t pid);
    [[nodiscard]] std::string
    render_rows(const Payload& payload, std::span<const LiveEntry> live, std::span<const ScriptState> scripts);
    [[nodiscard]] std::string render_help();
    [[nodiscard]] std::string render_usage();

}
