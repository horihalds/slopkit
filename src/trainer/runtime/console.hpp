#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "trainer/payload.hpp"
#include "trainer/runtime/resolve.hpp"
#include "trainer/runtime/rows.hpp"
#include "trainer/runtime/scripts.hpp"

namespace slopkit::trainer::console
{
    [[nodiscard]] std::string render_header(const Payload& payload, std::uint32_t pid);

    // One list line without its newline. The printed list and the interactive one
    // both render through these, so a row cannot look different in the two.
    [[nodiscard]] std::string render_value_row(const RowView& row);
    [[nodiscard]] std::string render_script_row(const RowView& row);
    [[nodiscard]] std::string render_value_row(const PayloadEntry& entry, const LiveEntry& state, std::size_t number);
    [[nodiscard]] std::string
    render_script_row(const PayloadScript& script, const ScriptState& state, std::size_t number);

    [[nodiscard]] std::string
    render_rows(const Payload& payload, std::span<const LiveEntry> live, std::span<const ScriptState> scripts);
    [[nodiscard]] std::string render_help();
    [[nodiscard]] std::string render_usage();

}
