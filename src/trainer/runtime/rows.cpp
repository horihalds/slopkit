#include "trainer/runtime/rows.hpp"

#include <format>
#include <string>

#include "scan/types.hpp"
#include "scan/value.hpp"

namespace slopkit::trainer
{
    namespace
    {
        std::string script_state(const ScriptState& state)
        {
            if (state.failed)
            {
                return "error";
            }
            return state.active ? "active" : "idle";
        }

        std::string script_interval(const ScriptState& state)
        {
            return state.interval_ms > 0 ? std::format("{} ms", state.interval_ms) : std::string {"-"};
        }

        std::string script_message(const ScriptState& state)
        {
            if (state.message.empty())
            {
                return "-";
            }
            std::string text = state.message;
            for (char& character : text)
            {
                if (character == '\n' || character == '\r')
                {
                    character = ' ';
                }
            }
            return text;
        }
    }

    RowView build_value_row(const PayloadEntry& entry, const LiveEntry& state, std::size_t number)
    {
        RowView row;
        row.kind        = RowKind::value;
        row.number      = number;
        row.label       = std::to_string(number);
        row.description = entry.description;
        row.address     = state.address == 0 ? std::string {"-"} : std::format("0x{:012x}", state.address);
        row.type        = std::string {scan::describe(entry.type)};
        row.value  = state.bytes.empty() ? std::string {"?"} : scan::format_value(entry.type, state.bytes, entry.hex);
        row.frozen = state.frozen;
        row.hex    = entry.hex;
        row.unresolved = !state.resolved;
        if (row.unresolved)
        {
            row.message = state.note;
        }
        return row;
    }

    RowView build_script_row(const PayloadScript& script, const ScriptState& state, std::size_t number)
    {
        RowView row;
        row.kind        = RowKind::script;
        row.number      = number;
        row.label       = std::format("S{}", number);
        row.description = script.description;
        row.active      = state.active;
        row.state       = script_state(state);
        row.interval    = script_interval(state);
        row.message     = script_message(state);
        return row;
    }

    std::vector<RowView>
    build_rows(const Payload& payload, std::span<const LiveEntry> live, std::span<const ScriptState> scripts)
    {
        std::vector<RowView> rows;
        rows.reserve(payload.entries.size() + payload.scripts.size());
        for (std::size_t position = 0; position < payload.entries.size() && position < live.size(); ++position)
        {
            rows.push_back(build_value_row(payload.entries[position], live[position], position + 1));
        }
        for (std::size_t position = 0; position < payload.scripts.size() && position < scripts.size(); ++position)
        {
            rows.push_back(build_script_row(payload.scripts[position], scripts[position], position + 1));
        }
        return rows;
    }

    std::optional<std::string> parse_value_text(const PayloadEntry& entry, std::string_view text)
    {
        const auto value = scan::parse_value(entry.type, text, entry.hex);
        if (!value)
        {
            return std::format("cannot read \"{}\" as {}: {}", text, scan::describe(entry.type), value.error().message);
        }
        return std::nullopt;
    }

}
