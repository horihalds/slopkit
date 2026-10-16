#include "trainer/runtime/console.hpp"

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "scan/types.hpp"
#include "scan/value.hpp"

namespace slopkit::trainer::console
{
    namespace
    {
        constexpr std::size_t kDescriptionWidth {24};

        std::string_view trim(std::string_view text)
        {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
            {
                text.remove_prefix(1);
            }
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
            {
                text.remove_suffix(1);
            }
            return text;
        }

        std::vector<std::string_view> words(std::string_view line)
        {
            std::vector<std::string_view> result;
            std::string_view              rest = trim(line);
            while (!rest.empty())
            {
                const auto space = rest.find_first_of(" \t");
                const auto word  = rest.substr(0, space);
                result.push_back(word);
                if (space == std::string_view::npos)
                {
                    break;
                }
                rest = trim(rest.substr(space + 1));
            }
            return result;
        }

        bool is_number(std::string_view text)
        {
            if (text.empty())
            {
                return false;
            }
            for (const char character : text)
            {
                if (character < '0' || character > '9')
                {
                    return false;
                }
            }
            return true;
        }

        bool parse_index(std::string_view text, std::size_t& index)
        {
            if (!is_number(text))
            {
                return false;
            }
            index = 0;
            for (const char character : text)
            {
                index = index * 10 + static_cast<std::size_t>(character - '0');
            }
            return index > 0;
        }

        std::string joined(const std::vector<std::string_view>& parts, std::size_t first)
        {
            std::string text;
            for (std::size_t position = first; position < parts.size(); ++position)
            {
                if (!text.empty())
                {
                    text += ' ';
                }
                text += parts[position];
            }
            return text;
        }

        Command simple_command(CommandKind kind)
        {
            Command command;
            command.kind = kind;
            return command;
        }

        Command invalid(std::string_view text)
        {
            Command command;
            command.kind = CommandKind::invalid;
            command.text = std::string {text};
            return command;
        }

        Command freeze_command(std::size_t index)
        {
            Command command;
            command.kind  = CommandKind::freeze;
            command.index = index;
            return command;
        }

        Command set_command(std::size_t index, std::string value)
        {
            Command command;
            command.kind  = CommandKind::set_value;
            command.index = index;
            command.value = std::move(value);
            return command;
        }

        std::string shorten(std::string_view text)
        {
            if (text.size() <= kDescriptionWidth)
            {
                return std::string {text};
            }
            return std::format("{}...", text.substr(0, kDescriptionWidth - 3));
        }

        std::string annotations(const PayloadEntry& entry, const LiveEntry& live)
        {
            std::string text;
            if (live.frozen)
            {
                text += " frozen";
            }
            if (entry.hex)
            {
                text += " hex";
            }
            if (!live.resolved)
            {
                text += " unresolved";
                if (!live.note.empty())
                {
                    text += std::format(" ({})", live.note);
                }
            }
            return text;
        }
    }

    Command parse_command(std::string_view line)
    {
        const auto parts = words(line);
        if (parts.empty())
        {
            return Command {};
        }

        const auto head = parts.front();
        if (head == "q" || head == "quit")
        {
            return simple_command(CommandKind::quit);
        }
        if (head == "r" || head == "refresh")
        {
            return simple_command(CommandKind::refresh);
        }
        if (head == "l" || head == "list")
        {
            return simple_command(CommandKind::list);
        }
        if (head == "h" || head == "help" || head == "?")
        {
            return simple_command(CommandKind::help);
        }

        if (head.front() == 'f')
        {
            std::size_t index = 0;
            if (parse_index(head.substr(1), index))
            {
                return freeze_command(index);
            }
            if (head == "f" || head == "freeze")
            {
                if (parts.size() < 2 || !parse_index(parts[1], index))
                {
                    return invalid(parts.size() < 2 ? head : parts[1]);
                }
                return freeze_command(index);
            }
            return invalid(head);
        }

        if (head.front() == 's')
        {
            const auto rest = head.substr(1);
            if (const auto equals = rest.find('='); equals != std::string_view::npos)
            {
                std::size_t index = 0;
                if (!parse_index(rest.substr(0, equals), index))
                {
                    return invalid(head);
                }
                return set_command(index, std::string {rest.substr(equals + 1)});
            }

            std::size_t index = 0;
            if (parse_index(rest, index))
            {
                if (parts.size() < 2)
                {
                    return invalid(head);
                }
                return set_command(index, joined(parts, 1));
            }
            if (head == "s" || head == "set")
            {
                if (parts.size() < 3 || !parse_index(parts[1], index))
                {
                    return invalid(parts.size() < 2 ? head : parts[1]);
                }
                return set_command(index, joined(parts, 2));
            }
            return invalid(head);
        }

        return invalid(std::string {trim(line)});
    }

    std::string render_header(const Payload& payload, std::uint32_t pid)
    {
        const auto name = payload.process_name.empty() ? std::string {"the target"} : payload.process_name;
        return std::format("slopkit-trainer: {} (pid {}), {} entr{}, {}-byte pointers\n",
                           name,
                           pid,
                           payload.entries.size(),
                           payload.entries.size() == 1 ? "y" : "ies",
                           payload.pointer_size);
    }

    std::string render_rows(const Payload& payload, std::span<const LiveEntry> live)
    {
        std::string text;
        for (std::size_t position = 0; position < payload.entries.size() && position < live.size(); ++position)
        {
            const auto& entry = payload.entries[position];
            const auto& state = live[position];

            const auto address = state.address == 0 ? std::string {"-"} : std::format("0x{:012x}", state.address);
            const auto value =
                state.bytes.empty() ? std::string {"?"} : scan::format_value(entry.type, state.bytes, entry.hex);

            text += std::format("{:>3}  {:<24}  {:<14}  {:<9}  {:<18}{}\n",
                                position + 1,
                                shorten(entry.description),
                                address,
                                scan::describe(entry.type),
                                value,
                                annotations(entry, state));
        }
        return text;
    }

    std::string render_help()
    {
        return "  f<N>          freeze or unfreeze entry N\n"
               "  s <N> <value> write a value (hex honouring the entry's hex flag)\n"
               "  r             re-resolve every expression\n"
               "  l             redraw the list\n"
               "  q             quit\n";
    }

    std::string render_usage()
    {
        return "usage: slopkit-trainer [--pid N | --name NAME] [--interval MS] [--list]\n"
               "                       [--wait MS] [--set N=VALUE]... [--freeze N]...\n"
               "\n"
               "Without attachment flags the trainer attaches to the process name recorded\n"
               "when it was generated. Actions run before the list is printed; without any\n"
               "action flag an interactive console is shown when stdin is a terminal.\n";
    }

}
