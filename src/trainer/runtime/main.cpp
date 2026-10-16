#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "scan/types.hpp"
#include "scan/value.hpp"
#include "trainer/payload.hpp"
#include "trainer/runtime/console.hpp"
#include "trainer/runtime/payload_reader.hpp"
#include "trainer/runtime/resolve.hpp"
#include "trainer/runtime/target.hpp"

namespace slopkit::trainer
{
    namespace
    {
        struct Options
        {
            std::uint32_t                                    pid {};
            bool                                             has_pid {false};
            std::string                                      name;
            std::uint32_t                                    interval_ms {250};
            std::uint32_t                                    wait_ms {0};
            bool                                             list {false};
            std::vector<std::size_t>                         freeze;
            std::vector<std::pair<std::size_t, std::string>> set_values;
            bool                                             help {false};

            [[nodiscard]] bool has_actions() const
            {
                return list || wait_ms > 0 || !freeze.empty() || !set_values.empty();
            }
        };

        std::optional<std::uint32_t> parse_number(std::string_view text)
        {
            std::uint32_t value  = 0;
            const auto    parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size())
            {
                return std::nullopt;
            }
            return value;
        }

        std::expected<std::string_view, std::string>
        next_value(int& index, int argc, char** argv, std::string_view flag)
        {
            if (index + 1 >= argc)
            {
                return std::unexpected(std::format("{} needs a value", flag));
            }
            ++index;
            return std::string_view {argv[index]};
        }

        std::expected<std::uint32_t, std::string> number_value(int& index, int argc, char** argv, std::string_view flag)
        {
            const auto text = next_value(index, argc, argv, flag);
            if (!text)
            {
                return std::unexpected(text.error());
            }
            const auto number = parse_number(*text);
            if (!number || *number == 0)
            {
                return std::unexpected(std::format("{} needs a positive number, got \"{}\"", flag, *text));
            }
            return *number;
        }

        std::expected<Options, std::string> parse_arguments(int argc, char** argv)
        {
            Options options;
            for (int index = 1; index < argc; ++index)
            {
                const std::string_view argument {argv[index]};
                if (argument == "--help" || argument == "-h")
                {
                    options.help = true;
                    continue;
                }
                if (argument == "--list")
                {
                    options.list = true;
                    continue;
                }
                if (argument == "--pid")
                {
                    const auto number = number_value(index, argc, argv, argument);
                    if (!number)
                    {
                        return std::unexpected(number.error());
                    }
                    options.pid     = *number;
                    options.has_pid = true;
                    continue;
                }
                if (argument == "--name")
                {
                    const auto value = next_value(index, argc, argv, argument);
                    if (!value)
                    {
                        return std::unexpected(value.error());
                    }
                    options.name = std::string {*value};
                    continue;
                }
                if (argument == "--interval")
                {
                    const auto number = number_value(index, argc, argv, argument);
                    if (!number)
                    {
                        return std::unexpected(number.error());
                    }
                    options.interval_ms = *number;
                    continue;
                }
                if (argument == "--wait")
                {
                    const auto text = next_value(index, argc, argv, argument);
                    if (!text)
                    {
                        return std::unexpected(text.error());
                    }
                    const auto number = parse_number(*text);
                    if (!number)
                    {
                        return std::unexpected(std::format("--wait needs a number, got \"{}\"", *text));
                    }
                    options.wait_ms = *number;
                    continue;
                }
                if (argument == "--freeze")
                {
                    const auto number = number_value(index, argc, argv, argument);
                    if (!number)
                    {
                        return std::unexpected(number.error());
                    }
                    options.freeze.push_back(*number);
                    continue;
                }
                if (argument == "--set")
                {
                    const auto value = next_value(index, argc, argv, argument);
                    if (!value)
                    {
                        return std::unexpected(value.error());
                    }
                    const auto equals = value->find('=');
                    if (equals == std::string_view::npos)
                    {
                        return std::unexpected(std::format("--set needs N=VALUE, got \"{}\"", *value));
                    }
                    const auto number = parse_number(value->substr(0, equals));
                    if (!number || *number == 0)
                    {
                        return std::unexpected(std::format("--set needs N=VALUE, got \"{}\"", *value));
                    }
                    options.set_values.emplace_back(*number, std::string {value->substr(equals + 1)});
                    continue;
                }
                return std::unexpected(std::format("unknown option \"{}\"", argument));
            }
            return options;
        }

        std::vector<LiveEntry> seed(const Payload& payload)
        {
            std::vector<LiveEntry> live;
            live.reserve(payload.entries.size());
            for (const auto& entry : payload.entries)
            {
                LiveEntry state;
                state.address = entry.last_address;
                state.frozen  = entry.frozen;
                live.push_back(std::move(state));
            }
            return live;
        }

        void refresh_all(const Payload& payload, Resolver& resolver, std::vector<LiveEntry>& live)
        {
            for (std::size_t position = 0; position < payload.entries.size(); ++position)
            {
                resolver.refresh(payload.entries[position], live[position]);
            }
        }

        void rewrite_frozen(const Payload& payload, Resolver& resolver, std::vector<LiveEntry>& live)
        {
            for (std::size_t position = 0; position < payload.entries.size(); ++position)
            {
                if (!live[position].frozen || live[position].bytes.empty())
                {
                    continue;
                }
                static_cast<void>(resolver.write(payload.entries[position], live[position], live[position].bytes));
            }
        }

        std::optional<std::string>
        apply_set(Resolver& resolver, const PayloadEntry& entry, LiveEntry& live, std::string_view text)
        {
            const auto value = scan::parse_value(entry.type, text, entry.hex);
            if (!value)
            {
                return std::format(
                    "cannot read \"{}\" as {}: {}", text, scan::describe(entry.type), value.error().message);
            }

            const auto bytes = scan::encode_value(entry.type, *value);
            if (const auto written = resolver.write(entry, live, bytes); !written)
            {
                return written.error();
            }

            live.bytes = bytes;
            live.note.clear();
            return std::nullopt;
        }

        void pump(std::uint32_t           wait_ms,
                  std::uint32_t           interval_ms,
                  const Payload&          payload,
                  Resolver&               resolver,
                  std::vector<LiveEntry>& live)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds {wait_ms};
            while (std::chrono::steady_clock::now() < deadline)
            {
                rewrite_frozen(payload, resolver, live);
                refresh_all(payload, resolver, live);

                const auto left =
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                        .count();
                const auto step =
                    std::min<std::uint32_t>(interval_ms, static_cast<std::uint32_t>(std::max<std::int64_t>(left, 0)));
                std::this_thread::sleep_for(std::chrono::milliseconds {step});
            }
        }

        int interactive(const Payload&          payload,
                        Resolver&               resolver,
                        std::vector<LiveEntry>& live,
                        std::uint32_t           pid,
                        std::uint32_t           interval_ms)
        {
            auto next_draw = std::chrono::steady_clock::time_point {};

            while (true)
            {
                const auto now = std::chrono::steady_clock::now();
                if (now >= next_draw)
                {
                    rewrite_frozen(payload, resolver, live);
                    refresh_all(payload, resolver, live);

                    clear_screen();
                    std::cout << console::render_header(payload, pid) << console::render_rows(payload, live)
                              << "slopkit-trainer> " << std::flush;
                    next_draw = now + std::chrono::milliseconds {interval_ms};
                }

                const auto input = read_line(interval_ms);
                if (input.status == InputStatus::closed)
                {
                    break;
                }
                if (input.status == InputStatus::idle)
                {
                    continue;
                }

                const auto command = console::parse_command(input.line);
                switch (command.kind)
                {
                case console::CommandKind::none:
                case console::CommandKind::list:
                    break;
                case console::CommandKind::quit:
                    return kExitOk;
                case console::CommandKind::help:
                    std::cout << console::render_help();
                    break;
                case console::CommandKind::refresh:
                    resolver.refresh_modules();
                    refresh_all(payload, resolver, live);
                    rewrite_frozen(payload, resolver, live);
                    break;
                case console::CommandKind::freeze:
                case console::CommandKind::set_value:
                case console::CommandKind::invalid:
                    break;
                }

                if (command.kind == console::CommandKind::invalid)
                {
                    std::cout << std::format("unrecognized input \"{}\"; press ? for help\n", command.text);
                    next_draw = {};
                    continue;
                }
                if (command.kind != console::CommandKind::freeze && command.kind != console::CommandKind::set_value)
                {
                    next_draw = {};
                    continue;
                }
                if (command.index == 0 || command.index > payload.entries.size())
                {
                    std::cout << std::format("this trainer carries {} entries, so there is no entry {}\n",
                                             payload.entries.size(),
                                             command.index);
                    next_draw = {};
                    continue;
                }

                const auto position = command.index - 1;
                if (command.kind == console::CommandKind::freeze)
                {
                    auto& state  = live[position];
                    state.frozen = !state.frozen;
                    if (state.frozen && !state.bytes.empty())
                    {
                        if (const auto written = resolver.write(payload.entries[position], state, state.bytes);
                            !written)
                        {
                            std::cout << std::format("{}\n", written.error());
                        }
                    }
                }
                else if (const auto message =
                             apply_set(resolver, payload.entries[position], live[position], command.value);
                         message)
                {
                    std::cout << std::format("{}\n", *message);
                }

                next_draw = {};
            }

            return kExitOk;
        }

        int run(const Options& options)
        {
            const auto payload = load_own_payload();
            if (!payload)
            {
                std::cerr << "slopkit-trainer: " << payload.error() << "\n";
                return kExitNoPayload;
            }
            if (payload->entries.empty())
            {
                std::cerr << "slopkit-trainer: this trainer carries no entries\n";
                return kExitNoPayload;
            }

            AttachRequest request;
            if (options.has_pid)
            {
                request.pid = options.pid;
            }
            request.name =
                request.pid != 0 ? std::string {} : (options.name.empty() ? payload->process_name : options.name);
            if (request.pid == 0 && request.name.empty())
            {
                std::cerr << "slopkit-trainer: no process name was recorded; pass --pid or --name\n";
                return kExitUsage;
            }

            const auto target = attach(request);
            if (!target)
            {
                std::cerr << "slopkit-trainer: " << target.error().message << "\n";
                return target.error().not_found ? kExitNoProcess : kExitAccess;
            }

            Resolver               resolver {**target, payload->pointer_size};
            std::vector<LiveEntry> live = seed(*payload);
            refresh_all(*payload, resolver, live);

            for (const auto index : options.freeze)
            {
                if (index == 0 || index > live.size())
                {
                    std::cerr << std::format(
                        "slopkit-trainer: this trainer carries {} entries, so there is no entry {}\n",
                        live.size(),
                        index);
                    return kExitUsage;
                }
                live[index - 1].frozen = true;
            }
            rewrite_frozen(*payload, resolver, live);

            for (const auto& [index, text] : options.set_values)
            {
                if (index == 0 || index > live.size())
                {
                    std::cerr << std::format(
                        "slopkit-trainer: this trainer carries {} entries, so there is no entry {}\n",
                        live.size(),
                        index);
                    return kExitUsage;
                }
                if (const auto message = apply_set(resolver, payload->entries[index - 1], live[index - 1], text))
                {
                    std::cerr << "slopkit-trainer: " << *message << "\n";
                    return kExitAccess;
                }
            }

            if (options.wait_ms > 0)
            {
                pump(options.wait_ms, options.interval_ms, *payload, resolver, live);
            }

            if (options.has_actions() || !stdin_is_terminal())
            {
                std::cout << console::render_header(*payload, (*target)->pid()) << console::render_rows(*payload, live);
                return kExitOk;
            }

            return interactive(*payload, resolver, live, (*target)->pid(), options.interval_ms);
        }
    }

    int run(int argc, char** argv)
    {
        const auto options = parse_arguments(argc, argv);
        if (!options)
        {
            std::cerr << "slopkit-trainer: " << options.error() << "\n\n" << console::render_usage();
            return kExitUsage;
        }
        if (options->help)
        {
            std::cout << console::render_usage();
            return kExitOk;
        }
        return run(*options);
    }

}

int main(int argc, char** argv)
{
    return slopkit::trainer::run(argc, argv);
}
