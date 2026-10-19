#include "trainer/runtime/console.hpp"

#include <format>
#include <string>
#include <string_view>

namespace slopkit::trainer::console
{
    namespace
    {
        constexpr std::size_t kDescriptionWidth {24};

        std::string shorten(std::string_view text)
        {
            if (text.size() <= kDescriptionWidth)
            {
                return std::string {text};
            }
            return std::format("{}...", text.substr(0, kDescriptionWidth - 3));
        }

        std::string described(const RowView& row)
        {
            return std::string(static_cast<std::size_t>(row.depth) * 2, ' ') + shorten(row.description);
        }
    }

    std::string render_header(const Payload& payload, std::uint32_t pid)
    {
        const auto name   = payload.process_name.empty() ? std::string {"the target"} : payload.process_name;
        const auto status = pid == 0 ? std::string {"waiting for it to appear"} : std::format("pid {}", pid);
        return std::format("slopkit-trainer: {} ({}), {} entr{}, {} script{}, {}-byte pointers\n",
                           name,
                           status,
                           payload.value_count(),
                           payload.value_count() == 1 ? "y" : "ies",
                           payload.script_count(),
                           payload.script_count() == 1 ? "" : "s",
                           payload.pointer_size);
    }

    std::string render_value_row(const RowView& row)
    {
        std::string annotations;
        if (row.frozen)
        {
            annotations += " frozen";
        }
        if (row.hex)
        {
            annotations += " hex";
        }
        if (row.unresolved)
        {
            annotations += " unresolved";
            if (!row.message.empty())
            {
                annotations += std::format(" ({})", row.message);
            }
        }

        return std::format("{:>3}  {:<24}  {:<14}  {:<9}  {:<18}{}",
                           row.number,
                           described(row),
                           row.address,
                           row.type,
                           row.value,
                           annotations);
    }

    std::string render_script_row(const RowView& row)
    {
        return std::format(
            "S{}  {:<24}  {:<14}  {:<9}  {}", row.number, described(row), row.state, row.interval, row.message);
    }

    std::string render_value_row(const PayloadEntry& entry, const LiveEntry& state, std::size_t number)
    {
        return render_value_row(build_value_row(entry, state, number));
    }

    std::string render_script_row(const PayloadScript& script, const ScriptState& state, std::size_t number)
    {
        return render_script_row(build_script_row(script, state, number));
    }

    std::string
    render_rows(const Payload& payload, std::span<const LiveEntry> live, std::span<const ScriptState> scripts)
    {
        std::string text;
        for (const auto& row : build_rows(payload, live, scripts))
        {
            text += row.kind == RowKind::value ? render_value_row(row) : render_script_row(row);
            text += '\n';
        }
        return text;
    }

    std::string render_help()
    {
        return "The trainer lists the table's rows in the order it was saved, with a\n"
               "child indented under its parent.\n"
               "\n"
               "  Check column    tick a value entry to freeze or unfreeze it, or a script\n"
               "                  to activate it (running its activate hook) or deactivate\n"
               "                  it (running its deactivate hook).\n"
               "  Value column    double-click a value entry's cell, type a new value and\n"
               "                  press Enter; Esc or a click elsewhere discards the edit,\n"
               "                  and a text the entry's type or hex flag refuses keeps the\n"
               "                  editor open with the reason on the status bar.\n"
               "  Numbers         the CLI addresses a value entry as N (--set N, --freeze N)\n"
               "                  and a script as S<N> (--activate N); the two numberings are\n"
               "                  independent, and a row's number is in its tooltip.\n"
               "  File > Refresh now   re-resolve every expression.\n"
               "  File > Close         end the session (Ctrl+W).\n"
               "  Help > Help          this text (F1).\n"
               "  Help > About         the entry and script counts and the target.\n"
               "\n"
               "The status bar shows the trainer's header on the left and the last\n"
               "action or script message on the right, both refreshed by the live pass.\n";
    }

    std::string render_usage()
    {
        return "usage: slopkit-trainer [--pid N | --name NAME] [--interval MS] [--list]\n"
               "                       [--wait MS] [--set N=VALUE]... [--freeze N]...\n"
               "                       [--activate N]... [--verbose]\n"
               "\n"
               "Without attachment flags the trainer attaches to the process name recorded\n"
               "when it was generated. Actions run before the list is printed; without any\n"
               "action flag the trainer window opens when a display is available.\n"
               "A missing target keeps the window or a --wait run waiting, and the trainer\n"
               "attaches as soon as it appears.\n"
               "Scripts start idle; --activate N ticks script S<N> before the actions run.\n"
               "--verbose writes debug-level logging to <trainer>.log beside this file.\n";
    }

}
