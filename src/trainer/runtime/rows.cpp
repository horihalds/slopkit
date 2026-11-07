#include "trainer/runtime/rows.hpp"

#include <format>
#include <string>

#include "scan/types.hpp"
#include "scan/value.hpp"

namespace slopkit::trainer
{
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
            row.note = state.note;
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
        row.type        = "script";
        row.active      = state.active;
        row.failed      = state.failed;
        return row;
    }

    std::vector<RowView>
    build_rows(const Payload& payload, std::span<const LiveEntry> live, std::span<const ScriptState> scripts)
    {
        const auto           entries = payload.values();
        const auto           records = payload.script_records();
        std::vector<RowView> rows;
        rows.reserve(payload.items.size());

        std::size_t value_position  = 0;
        std::size_t script_position = 0;
        for (const auto& item : payload.items)
        {
            if (item.kind == ItemKind::value)
            {
                if (value_position < entries.size() && value_position < live.size())
                {
                    auto row  = build_value_row(*entries[value_position], live[value_position], value_position + 1);
                    row.depth = item.depth;
                    rows.push_back(std::move(row));
                }
                ++value_position;
            }
            else
            {
                if (script_position < records.size() && script_position < scripts.size())
                {
                    auto row =
                        build_script_row(records[script_position], scripts[script_position], script_position + 1);
                    row.depth = item.depth;
                    rows.push_back(std::move(row));
                }
                ++script_position;
            }
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
