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
    }

    std::string render_header(const Payload& payload, std::uint32_t pid)
    {
        const auto name   = payload.process_name.empty() ? std::string {"the target"} : payload.process_name;
        const auto status = pid == 0 ? std::string {"waiting for it to appear"} : std::format("pid {}", pid);
        return std::format("slopkit-trainer: {} ({}), {} entr{}, {} script{}, {}-byte pointers\n",
                           name,
                           status,
                           payload.entries.size(),
                           payload.entries.size() == 1 ? "y" : "ies",
                           payload.scripts.size(),
                           payload.scripts.size() == 1 ? "" : "s",
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
                           shorten(row.description),
                           row.address,
                           row.type,
                           row.value,
                           annotations);
    }

    std::string render_script_row(const RowView& row)
    {
        return std::format("S{}  {:<24}  {:<14}  {:<9}  {}",
                           row.number,
                           shorten(row.description),
                           row.state,
                           row.interval,
                           row.message);
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
        bool        printed_value = false;
        bool        printed_blank = false;
        for (const auto& row : build_rows(payload, live, scripts))
        {
            // Scripts are a second numbering space: the rows above stay 1..N and
            // a script is always addressed as S<n>, never by the entry number.
            if (row.kind == RowKind::script && printed_value && !printed_blank)
            {
                text += '\n';
                printed_blank = true;
            }
            text += row.kind == RowKind::value ? render_value_row(row) : render_script_row(row);
            text += '\n';
            printed_value = printed_value || row.kind == RowKind::value;
        }
        return text;
    }

    std::string render_help()
    {
        return "The trainer lists its value entries as 1..N and its scripts as S1..Sm;\n"
               "the two numberings are independent.\n"
               "\n"
               "  Value column    double-click a value entry's cell, type a new value and\n"
               "                  press Enter; Esc or a click elsewhere discards the edit,\n"
               "                  and a text the entry's type or hex flag refuses keeps the\n"
               "                  editor open with the reason on the status bar.\n"
               "  Frozen / Active tick a value entry to freeze or unfreeze it, or a script\n"
               "                  to activate it (running its activate hook) or deactivate\n"
               "                  it (running its deactivate hook).\n"
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
