#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "trainer/payload.hpp"
#include "trainer/runtime/resolve.hpp"
#include "trainer/runtime/scripts.hpp"

namespace slopkit::trainer
{
    enum class RowKind
    {
        value,
        script,
    };

    // One row as both front-ends render it: the console list turns these fields
    // into a line, the Qt model puts them in columns, so the two cannot drift.
    struct RowView
    {
        RowKind     kind {RowKind::value};
        std::size_t number {}; // 1-based inside the row's own kind
        std::string label;     // "1" / "S1"
        std::string description;
        std::string address; // resolved text, empty for scripts
        std::string type;    // empty for scripts
        std::string value;   // empty for scripts
        bool        frozen {false};
        bool        active {false}; // scripts only
        bool        hex {false};
        bool        unresolved {false};
        std::string state; // "idle" | "active" | "error"
        std::string interval;
        std::string message; // a script's last message or a value's unresolved note
    };

    [[nodiscard]] RowView build_value_row(const PayloadEntry& entry, const LiveEntry& state, std::size_t number);
    [[nodiscard]] RowView build_script_row(const PayloadScript& script, const ScriptState& state, std::size_t number);

    [[nodiscard]] std::vector<RowView>
    build_rows(const Payload& payload, std::span<const LiveEntry> live, std::span<const ScriptState> scripts);

    // The one parse the retired `s <N> <value>` command used and the value
    // editor validates with: nullopt when the entry accepts `text`, otherwise
    // the reason it does not.
    [[nodiscard]] std::optional<std::string> parse_value_text(const PayloadEntry& entry, std::string_view text);

}
